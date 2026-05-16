// scripting/ThemesScriptable.hpp ─────────────────────────────────────────────
//
// s223 m1 — fifth row-bound model Scriptable, third library-collection
// variant. Direct application of the pattern s221 m1 (swatches) and
// s222 m1 (styles) established; the theme commands already exist from
// S103 m2 (AddThemeCommand / UpdateThemeCommand / RemoveThemeCommand)
// and are used by ThemesPanel today.
//
// Wraps the active project's `ThemeLibrary` as a collection Scriptable
// under abbrev `themes`, materialising transient `themes.<id>` proxies
// for per-instance operations via the s216 m1 router hooks
// (Scriptable::can_resolve / proxy_for).
//
// ── How themes relate to styles (and where they differ) ────────────────────
//
// Same shape as StylesScriptable:
//   * Library collection, not SceneNode collection — addressing is
//     `project->themes.find_theme(id)`, no doc-tree walk.
//   * Two tiers — app (read-only) and user (per-project, fully mutable).
//     v1 has NO app themes (m_app_themes is empty at construction); the
//     two-list shape and is_built_in() predicate are in place for
//     forward-compat. Mutating verbs still refuse on app-tier ids in
//     case future curated app themes land; the guard costs nothing.
//   * Execute-then-push commands — Add/Update/RemoveThemeCommand all
//     carry the mutation in their execute() body, same as the style
//     equivalents.
//
// Differences from styles:
//   * No category-filter on the panel side. ThemesPanel renders the
//     full user-tier theme list as a flat vertical box (`m_library_list`)
//     and rebuilds on every library signal (added / changed / removed
//     all connect to `refresh()` which calls `rebuild_library_list()`).
//     Consequence: this Scriptable needs NO panel-side navigation step
//     after `new`/`duplicate`. The third application of the "library-
//     collection Scriptables drive panel visibility through the panel's
//     mechanism" canon entry — see "Panel visibility" block below.
//   * v1 has no app themes, so the app-tier guard is academic; mutating
//     verbs still refuse on `app:`-prefixed ids for forward-compat. The
//     duplicate verb still allows duplicating from either tier (also
//     academic today; the affordance is in place for when app themes
//     ship).
//   * Themes don't bind — there's no `bound_theme` SceneNode field, and
//     `apply` is one-shot. Out of m1 scope: an `apply` verb that drives
//     `apply_theme_to_doc(theme, doc, motif)`. Apply is NOT undoable in
//     v1 by design (Theme.hpp top-of-file rationale); exposing it from
//     a script requires deciding the doc-targeting model and bypassing
//     the panel's confirmation modal. Deferred to its own milestone.
//   * ThemesPanel's UI vocabulary doesn't surface a "category" affordance
//     today (no category UI in the m_library_list rebuild — themes are
//     listed flat). Theme.hpp DOES carry a `header.category` field,
//     and ThemeLibrary exposes `user_categories()` / `app_categories()`
//     for forward-compat with the (currently empty) future grouped-view.
//     The Scriptable surfaces the `category` verb (on both proxy and
//     collection) for parity with StylesScriptable — same
//     UpdateThemeCommand shape, mechanically free. If a future panel
//     revision grows category UI, the Scriptable doesn't need a
//     follow-up.
//
// ── Panel visibility — third instance, no fix-1 needed ─────────────────────
//
// The canon entry from s221 / s222 reads: "library-collection
// Scriptables must drive panel visibility through whatever the panel
// uses to filter, not just through library state." Two prior
// applications:
//
//   * s221 m1 (swatches): library-side fix. SwatchesPanel renders only
//     the currently-active palette's contents; a new swatch in the
//     library but not in the active palette is invisible. The Scriptable
//     mirrors SwatchesPanel::on_new_swatch's `add_to_palette` call so
//     the new swatch appears in the active palette grid. Fix lives at
//     the library layer because the panel observes via signal.
//
//   * s222 m1 (styles): panel-side fix. StylesPanel renders only the
//     chips in `m_active_category`; a new style in a different category
//     is invisible until the user picks that category in the dropdown.
//     The Scriptable calls `StylesPanel::set_active_category(cat, false)`
//     after `new`/`duplicate` so the user sees the result. Fix lives
//     at the panel layer because the filter is panel state, not
//     library state.
//
// Third application — themes:
//
// ThemesPanel renders the FULL user-tier theme list as a flat vertical
// box. No category filter, no palette selector, no chooser dropdown.
// Every library mutation signal connects directly to `refresh()` which
// calls `rebuild_library_list()` which walks `user_themes_in_category`
// for every distinct user category and emits one row per theme. A new
// theme added by AddThemeCommand fires signal_theme_added, refresh
// rebuilds, the new row appears. **No panel-side navigation step is
// needed because the panel doesn't filter anything.**
//
// The canon entry isn't "always do a fix-1 dance." It's "always check
// what the panel does for visibility, and match it — which may mean
// doing nothing, because the panel's mechanism IS just-rebuild-the-
// flat-list and the library signal already drives that." Three concrete
// shapes now:
//
//   * Library-side fix (swatches): mirror a panel-action library call.
//   * Panel-side fix (styles): set panel state directly.
//   * No fix (themes): panel auto-rebuilds the flat list; library
//                      signal drives it; nothing further to do.
//
// The construction surface is correspondingly simpler — two args
// (project getter + history) rather than three (no PanelGetter).
// Matches SwatchesScriptable's two-arg constructor; differs from
// StylesScriptable's three-arg.
//
// One subtle visibility nuance worth naming: ThemesPanel carries an
// `m_selected_id` for "which theme is the active apply source." It's a
// panel-side selection mark distinct from library state. Should
// scripted `new` / `duplicate` set this so the just-created theme
// becomes the active apply source? Decision: no. Setting it is an
// opinionated workflow choice (the user might be scripting a setup
// pass that just populates themes without any apply intent); the row
// IS visible without intervention, which is the only visibility-rule
// guarantee. If a script wants to mark a theme as the apply source,
// that's a candidate `select` verb in a future milestone, not a
// silent side-effect of `new`.
//
// ── Addressing ─────────────────────────────────────────────────────────────
//
//   themes                  — the collection Scriptable. Set verbs and
//                             set queries. ONE registry entry per app
//                             session; held as a member by MainWindow,
//                             registered for the lifetime of the window.
//
//   themes.<id>             — per-instance proxy, materialised on demand
//                             by the collection. Reads / writes the
//                             specific theme's name / category through
//                             the project's ThemeLibrary. NOT registered
//                             — the registry only knows about `themes`.
//                             A `list` from the Scripter shows `themes`
//                             but never shows `themes.<id>` entries
//                             (same pilot success condition as
//                             layers / guides / swatches / styles).
//
// ── Lifetime and the project pointer ───────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable / SwatchesScriptable
// / StylesScriptable: a project getter (`std::function<CurvzProject*()>`)
// resolved fresh on every verb call, not a captured raw pointer.
// MainWindow's m_project unique_ptr gets reset and reassigned at runtime
// (close-project, load-project); the getter survives that, the
// Scriptable does not need re-registration.
//
// MainWindow constructs the ThemesScriptable once (after m_project is
// initialised) and the registry holds the entry for the window's
// lifetime.
//
// ── Commands and undo ──────────────────────────────────────────────────────
//
// Every mutating theme verb pushes a `CommandHistory` command. S103 m2
// made all CRUD operations undoable from the panel; this Scriptable
// rides on that exact same plumbing:
//
//   new / duplicate    → AddThemeCommand (standard ctor, library mints
//                        the id; m_assigned_id captures it)
//   delete             → RemoveThemeCommand (full Theme snapshot — same
//                        shape as RemoveStyleCommand, the snapshot is
//                        the whole Theme value so undo can re-add the
//                        full sub-bundle state, not just the header)
//   rename             → UpdateThemeCommand carrying before/after with
//                        only header.name diffed
//   category (proxy)   → UpdateThemeCommand carrying before/after with
//                        only header.category diffed
//
// The push pattern is `cmd->execute(); m_history->push(std::move(cmd))`,
// matching ThemesPanel::on_save_current_as_theme / on_rename_theme /
// on_duplicate_theme / on_delete_theme's shape. The Theme commands all
// carry their mutation in execute(); we invoke execute() to do the
// mutation (and, for AddThemeCommand, to capture the minted id via
// m_assigned_id) before pushing.
//
// The `m_history` pointer is captured at construction and outlives the
// Scriptable (CommandHistory is a value member of MainWindow). Same
// non-owning lifetime story as the other model scriptables.
//
// What's NOT undoable through this Scriptable — `apply`. Themes don't
// bind; apply is one-shot and non-undoable in v1 (Theme.hpp top-of-file
// rationale). m1 doesn't expose an `apply` verb at all (out of scope).
// Library CRUD IS undoable; that's what this Scriptable covers.
//
// ── App-tier guard ─────────────────────────────────────────────────────────
//
// Every mutating verb refuses on an app-tier id, returning Null without
// pushing a command. The escape hatch is `duplicate` — duplicate any
// app theme to get a user-tier copy you can edit. v1 has no app themes
// so the guard is academic; the contract is in place for when curated
// app themes land (one-line edit to ThemeLibrary's constructor).
//
// Read queries work on both tiers: `count`, `all_ids`, `find_by_name`,
// proxy `name`/`category`/`is_built_in`/`iid` all resolve through
// ThemeLibrary::find_theme (user first, then app).
//
// App-vs-user partition is exposed through two parallel set queries:
// `user_ids` and `app_ids`. Scripts that want to iterate only the
// editable themes use `user_ids`; scripts that want a full picture use
// `all_ids`. In v1 with no app themes, `app_ids` always returns "" and
// `user_ids` == `all_ids`; the shape is in place.
//
// ── Verb / query surface ───────────────────────────────────────────────────
//
// Verb names come from the panel's own UI vocabulary (per-row icon
// buttons + library footer): `new`, `delete`, `duplicate`, `rename`,
// plus the `category` verb for parity with StylesScriptable (Theme
// carries the field for symmetry with Style even though no UI surfaces
// it yet). Same "verbs come from the application's UI vocabulary, not
// API conventions" rule established in s217 m2.
//
// COLLECTION VERBS (on `themes`):
//   new                                  — create a new user-tier theme.
//                                          Optional args:
//                                            args[0] = name (string,
//                                                      empty allowed)
//                                            args[1] = category (string,
//                                                      empty = uncategorised)
//                                          Returns the new id, or "" on
//                                          failure. Defaults: empty name,
//                                          empty category, all sub-bundles
//                                          at their struct defaults (the
//                                          "factory baseline" — applying
//                                          it would reset a doc to fresh-
//                                          out-of-the-box). Capture-from-
//                                          doc (the panel's [+] button
//                                          flow) is OUT of m1 scope; if
//                                          a script wants a theme that
//                                          mirrors a doc's current state,
//                                          that's a future `capture` verb.
//   delete "<id>"                        — remove a user-tier theme.
//                                          Refuses on app-tier ids
//                                          (returns Null without
//                                          pushing). Reads the live
//                                          Theme first to snapshot it
//                                          into RemoveThemeCommand;
//                                          same shape as RemoveStyleCommand,
//                                          carries the whole Theme value
//                                          (not just an id) so undo can
//                                          re-add the full sub-bundle
//                                          state.
//   duplicate "<id>"                     — duplicate a theme from either
//                                          tier into the user tier. The
//                                          duplicate has a fresh id;
//                                          " copy" is appended to the
//                                          name (empty name stays empty
//                                          — same shape as the panel's
//                                          on_duplicate_theme, minus the
//                                          panel's auto-dedupe walk
//                                          which is UX polish for the
//                                          modal). Category is preserved
//                                          verbatim from the source.
//                                          Returns the new id, or "" on
//                                          failure.
//   rename "<id>" "<name>"               — rename a user-tier theme.
//                                          Refuses on app-tier ids.
//                                          Pushes UpdateThemeCommand
//                                          with full before/after
//                                          snapshots; only header.name
//                                          differs. Skips the push if
//                                          name is unchanged (matches
//                                          the panel's "no-op on
//                                          unchanged" guard). Empty
//                                          name accepted at this layer
//                                          — the panel's empty-as-cancel
//                                          is a modal UX choice, not a
//                                          model rule.
//   category "<id>" "<cat>"              — set the category on a
//                                          user-tier theme. Refuses on
//                                          app-tier ids. Pushes
//                                          UpdateThemeCommand with full
//                                          before/after snapshots; only
//                                          header.category differs.
//                                          Empty cat is meaningful — it
//                                          marks the theme as
//                                          uncategorised. Note: today's
//                                          ThemesPanel doesn't render
//                                          a category-grouped view, so
//                                          the category field is
//                                          metadata-only from the UI's
//                                          perspective. The Scriptable
//                                          surfaces the verb anyway for
//                                          parity with StylesScriptable
//                                          and forward-compat with a
//                                          future grouped panel revision.
//   find_by_name "<name>"                — id of the first theme (any
//                                          tier) whose header.name
//                                          exactly matches. Same
//                                          first-hit contract as
//                                          StylesScriptable::find_by_name.
//                                          Returns "" on miss. Names
//                                          aren't unique by construction
//                                          (the library enforces
//                                          uniqueness on ids, not names
//                                          — has_user_name is a query
//                                          the panel uses for its own
//                                          auto-dedupe walk, not a
//                                          library-level uniqueness
//                                          constraint).
//
// COLLECTION QUERIES (on `themes`):
//   count                                — total number of themes
//                                          across BOTH tiers (app +
//                                          user). Sum of app_theme_count
//                                          and user_theme_count. In v1
//                                          with no app themes, equal to
//                                          user_theme_count.
//   all_ids                              — comma-separated ThemeIds in
//                                          library iteration order (app
//                                          first, then user, each in
//                                          insertion order within tier).
//                                          Same sentinel shape as
//                                          SwatchesScriptable / StylesScriptable.
//   user_ids                             — comma-separated ThemeIds in
//                                          the user tier only. Useful
//                                          when scripts want to iterate
//                                          only the editable themes
//                                          without tripping app-tier
//                                          refusal on every mutating
//                                          verb. Parallel to
//                                          StylesScriptable::user_ids;
//                                          named after the ThemeLibrary
//                                          term (`user_themes`,
//                                          `user_categories`,
//                                          `user_theme_count`).
//   app_ids                              — comma-separated ThemeIds in
//                                          the app tier only. Read-only
//                                          — every mutating verb
//                                          refuses on these. v1 always
//                                          returns "" (app list empty).
//                                          Parallel to
//                                          StylesScriptable::app_ids.
//
// ELEMENT VERBS (on `themes.<id>`):
//   rename "<name>"                      — write header.name. Refuses on
//                                          app-tier (returns Null).
//                                          Pushes UpdateThemeCommand.
//                                          Skip-no-op when name didn't
//                                          change.
//   category "<cat>"                     — write header.category. Refuses
//                                          on app-tier. Pushes
//                                          UpdateThemeCommand. Empty
//                                          cat is meaningful — moves
//                                          to the uncategorised bucket.
//                                          Skip-no-op when category
//                                          didn't change.
//
// ELEMENT QUERIES (on `themes.<id>`):
//   name           — string       theme.header.name
//   category       — string       theme.header.category (empty string
//                                  for uncategorised; same convention
//                                  as Style).
//   is_built_in    — bool         true iff the theme lives in the app
//                                  tier. Named after Theme.hpp's
//                                  is_built_in() free function and
//                                  ThemeLibrary::is_built_in() member.
//                                  v1 always false (no app themes); the
//                                  predicate is the canonical discriminant
//                                  vocabulary in the theme model. Compare
//                                  SwatchesScriptable's `is_default`,
//                                  which follows the swatches model's
//                                  vocabulary.
//   iid            — string       theme id (the addressing key). Named
//                                  `iid` for shape-symmetry with
//                                  LayerProxy / GuideProxy / SwatchProxy
//                                  / StyleProxy — every per-instance
//                                  proxy returns its addressing key
//                                  under `iid`.
//
// ── Out of scope for s223 m1 ───────────────────────────────────────────────
//
// **Sub-bundle fields** — Theme carries six sub-bundles (UnitSettings,
// MotifSettings, GuideSettings, GridSettings, MarginSettings,
// ThemeSnapSettings). The proxy surface in m1 covers only the header
// fields (name, category). Per-field verbs for unit / motif colours /
// grid spacing / margin top / snap toggles etc. can come later; the
// pattern is established and adding `set_unit`, `grid_enabled`,
// `motif_dark_artboard_r`, etc. is mechanical once the scope is opened.
// Same precedent as StylesScriptable's deferred fill/stroke/shadow —
// header-only first cut is the s222 m1 shape and a fine s223 m1 shape.
//
// **Apply** — `apply_theme_to_doc(theme, doc, motif)` writes a theme's
// sub-bundle values into a document's current settings. It is NOT
// undoable in v1 by explicit design (Theme.hpp). Exposing it as a
// script verb means deciding (a) the doc-targeting model (active doc?
// all docs? a named-doc list?), (b) confirmation skip (panel pops a
// modal; scripts probably shouldn't), and (c) interaction with the
// undo stack (a non-undoable verb in a chain of undoable verbs is a
// trap). Deferred — its own milestone.
//
// **Capture from doc** — the panel's `on_save_current_as_theme` flow
// reads `capture_theme_from_doc(active, motif)`, prompts for a name,
// pushes AddThemeCommand. A scripted equivalent would be a `capture`
// verb taking an optional doc-targeting arg. Same out-of-scope
// reasoning as apply — its own design call.
//
// **Theme editor dialog** — ThemesPanel::on_edit_theme emits a signal
// that opens a full ThemeEditDialog. That's a panel-side concern;
// the Scriptable's surface is library CRUD, not editor-dialog
// invocation. If a script needs to drive sub-bundle field edits, the
// per-field setter milestone (above) is the place for those, not a
// "show the dialog" verb.
//
// **Import / export** — the panel has a kebab menu for "Import themes…"
// / "Export themes…" calling `themes_io::import_themes` and
// `themes_io::export_themes`. Those operate on the file system and
// have file-picker UX. Scripted file IO is a separate surface (already
// at design discussion for other libraries); not a ThemesScriptable
// concern.
//
// ── On active-project scope ────────────────────────────────────────────────
//
// Same shape as SwatchesScriptable / StylesScriptable: all verbs operate
// on `project->themes` (the library hangs off the project, NOT off any
// specific document). Themes are project-scoped state; the "active
// document" doesn't enter into theme lookup. The `themes.<id>` proxy
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
}

namespace curvz::scripting {

class ThemesScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;

    // Registers as "themes" via the Scriptable base ctor. The project
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
    // pointer is safe for the ThemesScriptable's lifetime. Every
    // mutating verb DEREFERENCES this pointer (same shape as
    // SwatchesScriptable / StylesScriptable).
    //
    // No PanelGetter argument — see "Panel visibility" block above.
    // The ThemesPanel rebuilds its flat library list on every signal,
    // so library-side mutation is sufficient for the user to see new
    // rows. Construction surface is correspondingly two-arg
    // (matches SwatchesScriptable; differs from StylesScriptable's
    // three-arg).
    ThemesScriptable(ProjectGetter get_project,
                     Curvz::CommandHistory* history);
    ~ThemesScriptable() override = default;

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
