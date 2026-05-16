// scripting/ThemesScriptable.hpp ─────────────────────────────────────────────
//
// s223 m1 — fifth row-bound model Scriptable, third library-collection
// variant. Direct application of the pattern s221 m1 (swatches) and
// s222 m1 (styles) established; the theme commands already exist from
// S103 m2 (AddThemeCommand / UpdateThemeCommand / RemoveThemeCommand)
// and are used by ThemesPanel today.
//
// s226 m1 — per-field setter surface added. The s223 m1 header-only
// first cut deferred every sub-bundle field as backlog; s226 m1 closes
// that backlog. 35 new proxy verbs covering all six sub-bundles
// (UnitSettings, MotifSettings, GuideSettings, GridSettings,
// MarginSettings, ThemeSnapSettings), paired 1:1 with 35 reads. Direct
// application of the s225 m1 push_field_edit template helper — same
// shape, no new mechanisms. See "Appearance fields — added in s226 m1"
// block below for the full design rationale (hex-string composite
// shape for colours, lowercase enum vocabulary for unit, snap as bool
// toggles).
//
// s227 m1 — apply / capture verbs added. Last two pieces of the
// ThemesScriptable backlog (s223 m1 forward). Both operate on the
// active doc only in v1 (multi-doc targeting deferred until doc
// Scriptables exist). Apply is NOT undoable (preserves the
// Theme.hpp v1 design); capture pushes AddThemeCommand and IS
// undoable. Both refresh the canvas through a new
// ThemeLibrary::signal_theme_applied (apply) / the existing
// signal_theme_added (capture). See "Apply and capture — added in
// s227 m1" block below for the design rationale and the two open
// questions that collapsed (motif arg is no longer load-bearing
// after s183 m5a; doc-targeting is active-only with forward-compat
// for multi-doc args).
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
//                             specific theme's name / category / sub-
//                             bundle fields through the project's
//                             ThemeLibrary. NOT registered — the
//                             registry only knows about `themes`. A
//                             `list` from the Scripter shows `themes`
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
//   capture (s227 m1)  → AddThemeCommand (same as new, but the inserted
//                        Theme is capture_theme_from_doc(active) with the
//                        name/category set from args. The library mints
//                        the id same way.)
//   delete             → RemoveThemeCommand (full Theme snapshot — same
//                        shape as RemoveStyleCommand, the snapshot is
//                        the whole Theme value so undo can re-add the
//                        full sub-bundle state, not just the header)
//   rename             → UpdateThemeCommand carrying before/after with
//                        only header.name diffed
//   category (proxy)   → UpdateThemeCommand carrying before/after with
//                        only header.category diffed
//   sub-bundle field   → UpdateThemeCommand carrying before/after with
//   (s226 m1)            only that field diffed. Same execute-then-push
//                        shape via the `push_field_edit` template helper
//                        (lifted from StyleProxy s225 m1 — see Appearance
//                        fields block below).
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
// What's NOT undoable through this Scriptable — `apply` (s227 m1).
// Themes don't bind; apply is one-shot and non-undoable in v1
// (Theme.hpp top-of-file rationale). The script form skips command
// generation entirely — `themes.t apply` runs apply_theme_to_doc with
// no push. A Ctrl+Z over a script that includes apply will walk past
// the apply line silently. Script authors who need apply-with-undo
// will wait for the (out-of-m1-scope) EditDocStateCommand path.
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
// proxy `name`/`category`/`is_built_in`/`iid`/<sub-bundle reads> all
// resolve through ThemeLibrary::find_theme (user first, then app).
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
// Sub-bundle field verbs (s226 m1) follow `<bundle>_<field>` naming:
// `motif_dark_artboard`, `grid_spacing_x`, `snap_guides`, etc. The
// header.category verb keeps its bare name (no `header_` prefix) —
// "verbs come from the application's UI vocabulary" applied: `category`
// is the panel's term, not `header_category`. Sub-bundle fields don't
// have a comparable UI shortcut, so the bundle-qualified naming is the
// right shape for disambiguating (snap_enabled vs grid_enabled vs
// margin_enabled would all be three different "enabled" verbs otherwise).
// Same naming policy as the s225 m1 styles surface: shadow_dx is
// "shadow's dx field," not "shadow_dx" the unbroken token.
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
//                                          out-of-the-box). For a theme
//                                          that mirrors the active doc's
//                                          current state, use `capture`
//                                          instead (s227 m1).
//   capture                              — s227 m1. Capture the active
//   capture "<name>"                       doc's settings into a new
//   capture "<name>" "<cat>"               user-tier theme. Optional
//                                          args:
//                                            args[0] = name (string,
//                                                      empty → auto-
//                                                      generated "Theme N"
//                                                      via has_user_name
//                                                      walk, mirror of
//                                                      ThemesPanel
//                                                      on_save_current_as_theme)
//                                            args[1] = category (string,
//                                                      empty = uncategorised)
//                                          Returns the new id, or "" on
//                                          failure. Refuses (returns "")
//                                          if no project or no active doc.
//                                          Pushes AddThemeCommand —
//                                          undoable. See "Apply and
//                                          capture — added in s227 m1"
//                                          block below for the design
//                                          rationale (collection-only —
//                                          no proxy form; captures
//                                          dark-and-light pairs regardless
//                                          of current motif since s183 m5a
//                                          made the motif arg cosmetic).
//   apply "<id>"                         — s227 m1. Apply the named
//                                          theme to the project's active
//                                          doc. Refuses (returns Null)
//                                          on missing project, missing
//                                          active doc, or unknown id.
//                                          NO app-tier refusal — apply
//                                          is a non-mutating read of the
//                                          theme record (writes to the
//                                          doc, not the library), so the
//                                          tier guard that protects every
//                                          MUTATING verb doesn't apply.
//                                          NOT undoable in v1 by
//                                          Theme.hpp design; no command
//                                          is pushed. After apply: syncs
//                                          m_project->snap from the
//                                          active doc, then fires
//                                          ThemeLibrary::signal_theme_applied
//                                          which MainWindow's zone wiring
//                                          handles for canvas refresh +
//                                          inspector + schedule_save. The
//                                          proxy alternative
//                                          `themes.<id> apply` is also
//                                          available — both forms route
//                                          through the same shared code
//                                          path. See "Apply and capture
//                                          — added in s227 m1" block
//                                          below for the full design
//                                          rationale.
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
//   apply                                — s227 m1. Apply this theme
//                                          to the project's active doc.
//                                          No args. Refuses (returns
//                                          Null) on missing project or
//                                          missing active doc. NOT
//                                          subject to the head-of-invoke
//                                          app-tier guard — apply is a
//                                          non-mutating read of the
//                                          theme record (writes to the
//                                          doc, not the library), so it
//                                          sits BEFORE the guard in
//                                          invoke()'s dispatch order.
//                                          Forward-compat: when curated
//                                          app themes ship, applying
//                                          them via script works the
//                                          same way the panel's apply
//                                          button does (which doesn't
//                                          tier-check the source).
//                                          NOT undoable in v1
//                                          (Theme.hpp design); no
//                                          command is pushed. After
//                                          apply: syncs m_project->snap
//                                          from the active doc, then
//                                          fires
//                                          ThemeLibrary::signal_theme_applied
//                                          for the canvas refresh
//                                          cascade. Equivalent to the
//                                          collection form
//                                          `themes apply "<id>"`; both
//                                          share a helper. See "Apply
//                                          and capture — added in s227
//                                          m1" block below.
//
//   ── s226 m1 sub-bundle field setters ──
//   Every setter below pushes UpdateThemeCommand via push_field_edit
//   with the bundle-and-field-scoped description string ("Set theme
//   grid spacing x (script)", etc.). Skip-no-op guard fires before
//   the push. App-tier guard at head of invoke() refuses every field
//   setter on the same path as rename/category. Invalid input (bad
//   hex / unknown unit string) is silent no-op (mirror of styles
//   fill's malformed-spec behaviour).
//
//   UnitSettings:
//   unit "<px|in|mm|pt>"                 — s226 m1. Write
//                                          theme.units.display_unit.
//                                          Lowercase enum vocabulary
//                                          matching UnitSystem::label()
//                                          and Inspector's unit dropdown.
//                                          Unknown vocab is no-op.
//                                          Verb name drops the
//                                          `display_` prefix the model
//                                          field carries — same way
//                                          `category` verb drops
//                                          `header.`.
//
//   MotifSettings (six hex-string verbs, one per dark/light × region):
//   motif_dark_artboard "<#rrggbb>"      — s226 m1. RGB only (model has
//                                          no alpha channel for motif
//                                          colours); the parser accepts
//                                          rrggbbaa hex but the alpha
//                                          is discarded on write.
//   motif_dark_workspace "<#rrggbb>"     — s226 m1.
//   motif_dark_creation  "<#rrggbb>"     — s226 m1.
//   motif_light_artboard  "<#rrggbb>"    — s226 m1.
//   motif_light_workspace "<#rrggbb>"    — s226 m1.
//   motif_light_creation  "<#rrggbb>"    — s226 m1.
//
//   GuideSettings:
//   guide_color   "<#rrggbb>"            — s226 m1. RGB only (no alpha
//                                          channel on guide colour);
//                                          parser tolerates alpha,
//                                          discards on write.
//   guide_visible <bool>                 — s226 m1. Write
//                                          theme.guides.visible. This
//                                          is the SceneNode::visible
//                                          on the GuideLayer that apply
//                                          writes through; theme-level
//                                          gate.
//
//   GridSettings:
//   grid_enabled  <bool>                 — s226 m1. Write
//                                          theme.grid.enabled. Layer-
//                                          presence gate (apply uses
//                                          this to ensure / remove the
//                                          GridLayer).
//   grid_visible  <bool>                 — s226 m1. Write
//                                          theme.grid.visible.
//   grid_spacing_x <double>              — s226 m1.
//   grid_spacing_y <double>              — s226 m1.
//   grid_offset_x  <double>              — s226 m1.
//   grid_offset_y  <double>              — s226 m1.
//   grid_color    "<#rrggbb[aa]>"        — s226 m1. RGBA composite —
//                                          grid is the first sub-bundle
//                                          colour with an alpha channel.
//                                          Hex shape matches s225 m1
//                                          shadow_color.
//   grid_dots     <bool>                 — s226 m1. Write
//                                          theme.grid.dots. false=lines,
//                                          true=dots at intersections.
//
//   MarginSettings:
//   margin_enabled <bool>                — s226 m1. Layer-presence gate
//                                          (apply uses this to ensure /
//                                          remove the MarginLayer).
//   margin_visible <bool>                — s226 m1.
//   margin_top     <double>              — s226 m1.
//   margin_bottom  <double>              — s226 m1.
//   margin_left    <double>              — s226 m1.
//   margin_right   <double>              — s226 m1.
//   margin_columns <int>                 — s226 m1. The model field is
//                                          int (not double); the
//                                          Scriptable accepts Int / Double
//                                          via arg_as_int and rounds
//                                          toward zero. No clamp (the
//                                          inspector spinner enforces
//                                          >= 1 but the model accepts
//                                          0 / negative without UB —
//                                          same posture as styles
//                                          stroke_width).
//   margin_col_gap <double>              — s226 m1.
//   margin_rows    <int>                 — s226 m1.
//   margin_row_gap <double>              — s226 m1.
//   margin_color  "<#rrggbb[aa]>"        — s226 m1. RGBA composite,
//                                          same shape as grid_color.
//
//   ThemeSnapSettings (alias to CurvzDocument's SnapSettings):
//   snap_enabled  <bool>                 — s226 m1. The overall snap
//                                          gate (snap on/off as a whole).
//   snap_guides   <bool>                 — s226 m1.
//   snap_grid     <bool>                 — s226 m1.
//   snap_margins  <bool>                 — s226 m1.
//   snap_nodes    <bool>                 — s226 m1.
//   snap_edges    <bool>                 — s226 m1.
//   snap_centers  <bool>                 — s226 m1.
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
//   ── s226 m1 sub-bundle field reads ──
//   Every setter has a paired reader of the same name; the reader
//   returns the same shape the setter accepts (round-trippable). Hex
//   reads always emit lowercase ("#rrggbb" or "#rrggbbaa"); unit
//   reads emit lowercase ("px" / "in" / "mm" / "pt").
//
//   unit                            — string  "<px|in|mm|pt>"
//   motif_dark_artboard             — string  "#rrggbb" (alpha = 1.0,
//                                              hex output omits suffix)
//   motif_dark_workspace            — string  "#rrggbb"
//   motif_dark_creation             — string  "#rrggbb"
//   motif_light_artboard            — string  "#rrggbb"
//   motif_light_workspace           — string  "#rrggbb"
//   motif_light_creation            — string  "#rrggbb"
//   guide_color                     — string  "#rrggbb"
//   guide_visible                   — bool
//   grid_enabled                    — bool
//   grid_visible                    — bool
//   grid_spacing_x                  — double
//   grid_spacing_y                  — double
//   grid_offset_x                   — double
//   grid_offset_y                   — double
//   grid_color                      — string  "#rrggbb[aa]"
//   grid_dots                       — bool
//   margin_enabled                  — bool
//   margin_visible                  — bool
//   margin_top                      — double
//   margin_bottom                   — double
//   margin_left                     — double
//   margin_right                    — double
//   margin_columns                  — int
//   margin_col_gap                  — double
//   margin_rows                     — int
//   margin_row_gap                  — double
//   margin_color                    — string  "#rrggbb[aa]"
//   snap_enabled                    — bool
//   snap_guides                     — bool
//   snap_grid                       — bool
//   snap_margins                    — bool
//   snap_nodes                      — bool
//   snap_edges                      — bool
//   snap_centers                    — bool
//
// ── Appearance fields — added in s226 m1 ───────────────────────────────────
//
// The s223 m1 header-only first cut deferred every sub-bundle field as
// backlog. s226 m1 closes that backlog with 35 proxy verbs covering all
// six sub-bundles. The surface is proxy-only — the chip-context-menu
// fork that justifies collection-level `rename` / `category` doesn't
// extend to inspector edits like grid spacing or snap toggles. Scripts
// that have an id in hand and want to set fields use the dotted form:
//
//   themes new "Print Setup" "Workflow"
//   set t to result
//   themes.t unit "in"
//   themes.t grid_enabled true
//   themes.t grid_spacing_x 36.0
//   themes.t snap_guides false
//
// Every field write mirrors the proxy `rename`/`category` shape — full
// before/after Theme snapshots into UpdateThemeCommand via the
// push_field_edit template helper (lifted from StyleProxy s225 m1),
// skip-no-op when the value didn't change, app-tier guard at the head
// of invoke().
//
// **Hex composite shape for colours.** The six MotifSettings RGB
// triples, GuideSettings RGB, GridSettings RGBA, and MarginSettings
// RGBA all surface as hex strings rather than per-channel scalars.
// Trade-off:
//   * Pro: 9 colour verbs instead of 30 channel verbs. Symmetry with
//          s225 m1 `shadow_color`. Hex is what a designer typing into
//          a script will already have in their colour-picker workflow.
//   * Con: Can't say "leave the colour alone, just bump the alpha."
//          Neither could s225's shadow_color, and that's been fine.
//          A follow-up `*_alpha` companion verb could land if a use
//          case surfaces.
//
// RGB-only fields (motif × 6, guide) discard any alpha channel on
// write — the parser accepts "#rrggbbaa" but only the rgb part is
// stored. Reads always emit the no-alpha form. RGBA fields (grid,
// margin) round-trip the alpha; the read form includes the alpha
// suffix iff a < 1.0 (same to_hex shape as styles' shadow_color).
//
// **Cross-doc walk: trivially academic for v1.** Style-driven appearance
// edits ripple to bound SceneNodes via signal_style_changed since
// S81 m3d. Theme commands also fire signal_theme_changed, but themes
// don't bind (`apply_theme_to_doc` is one-shot — see Theme.hpp top of
// file). signal_theme_changed → ThemesPanel::refresh which rebuilds
// the panel's row list, and is also visible to anyone listening for
// theme library changes externally (none in v1). Script-driven
// theme edits update the library; apply-to-doc happens separately
// when the user (or a future apply verb) drives it.
//
// **Visual half of convergence — narrower than styles.** For s225 m1
// styles, Scott had the Style Editor open on a test style and saw
// every field update visibly per line — the StylesPanel's chip
// thumbnail re-renders on signal_style_changed and the editor's
// surrounding panel stays in sync. For themes, ThemesPanel's per-
// theme row only displays the theme name + edit/dup/del buttons,
// NOT the sub-bundle values. ThemeEditDialog DOES display them but
// doesn't observe signal_theme_changed (it's a buffer-then-commit
// dialog; live observation would conflict with its working buffer
// model). Consequence: scripted field edits ARE reflected in the
// library and ARE round-trip-readable via `get`, but a dialog open on
// the same theme during the script will NOT update its widgets live.
// The eyes-half check is "open the dialog AFTER the script, on the
// edited theme, and see the values reflected" — not "watch the dialog
// update line-by-line." See smoke test top comment for the operator's
// workflow.
//
// ── Out of scope for s226 m1 (CLOSED in s227 m1 — see below) ───────────────
//
// **Apply** — CLOSED in s227 m1. `apply_theme_to_doc(theme, doc, motif)`
// writes a theme's sub-bundle values into a document's current settings.
// It is NOT undoable in v1 by explicit design (Theme.hpp). The s226 m1
// header listed three open design questions; s227 m1 resolved them:
//
//   (a) Doc-targeting model: active doc only in v1. No args. Multi-doc
//       targeting (`themes.<id> apply "doc1,doc2"`) deferred until doc
//       Scriptables exist; the verb signature accepts no targets today
//       so a future args form extends forward-compatibly.
//
//   (b) Confirmation skip: scripts skip the panel's confirmation modal
//       (no question). Scripts run unattended.
//
//   (c) Motif disambiguation: DISSOLVED by s183 m5a. The funnel's
//       `current_motif` arg is no longer load-bearing — capture writes
//       both dark and light pairs from the doc directly, apply writes
//       both pairs back. The Scriptable still passes m_project->motif
//       for API stability, but the value doesn't affect output.
//
//   (d) Non-undoable-in-an-undoable-chain: accepted as documented
//       script-author-beware behaviour. Ctrl+Z over a chain that
//       includes apply will skip the apply line (no command to undo)
//       and undo the surrounding lines — same behaviour an inline
//       file-system operation would have. The cleaner alternative
//       (EditDocStateCommand pushing a doc-snapshot pair) remains an
//       independent backlog item, not blocking apply's first ship.
//
// **Capture from doc** — CLOSED in s227 m1. The panel's
// `on_save_current_as_theme` flow reads `capture_theme_from_doc(active,
// motif)` then prompts for a name. The scripted equivalent is the
// collection-level `themes capture` verb: same funnel, no name prompt
// (the script passes the name as an arg or accepts the auto-generated
// `Theme N` default that mirrors the panel's `has_user_name` walk).
// IS undoable — pushes AddThemeCommand the same way `themes new` does.
//
// **Theme editor dialog** — ThemesPanel::on_edit_theme emits a signal
// that opens a full ThemeEditDialog. That's a panel-side concern;
// the Scriptable's surface is library CRUD + sub-bundle field edits,
// not editor-dialog invocation. If a script needs to drive the
// dialog open/close, that's an inspector-tier verb on a future
// theme-editor Scriptable, not on the library Scriptable.
//
// **Import / export** — the panel has a kebab menu for "Import themes…"
// / "Export themes…" calling `themes_io::import_themes` and
// `themes_io::export_themes`. Those operate on the file system and
// have file-picker UX. Scripted file IO is a separate surface (already
// at design discussion for other libraries); not a ThemesScriptable
// concern.
//
// **Per-channel companion verbs** — see the hex-vs-channels trade-off
// in the Appearance fields block above. If a use case surfaces for
// "bump just the alpha" / "set just the green channel," follow-up
// `*_alpha` / `*_r` / `*_g` / `*_b` companion verbs could land. Today
// the hex composite is the only surface.
//
// ── Apply and capture — added in s227 m1 ───────────────────────────────────
//
// Two verbs that bridge the library and the active document, closing the
// last two ThemesScriptable backlog items (s223 m1 forward).
//
// **apply** — proxy form `themes.<id> apply` and collection form
// `themes apply "<id>"`. Both target the project's active doc; no
// multi-doc args in v1.
//
// What it does:
//   1. Refuses if no project or no active doc (returns Null, no side
//      effect — same defensive pattern as every other verb's "missing
//      preconditions" branch).
//   2. Allows app-tier ids. Apply is a non-mutating read of the theme
//      record (it writes to the doc, not to the library), so the
//      tier guard that protects every MUTATING verb in this Scriptable
//      doesn't apply. In the proxy form, the `apply` branch sits
//      ABOVE the head-of-invoke app-tier guard; in the collection
//      form, the `apply` branch simply doesn't check is_built_in.
//      v1 has no app themes so the practical behaviour is unchanged;
//      the explicit non-refusal records the design intent. Forward-
//      compat: when curated app themes ship, applying them by-script
//      works the same way the panel's apply button does.
//   3. Calls apply_theme_to_doc(theme, *active_doc, m_project->motif).
//      The motif arg is no longer load-bearing (s183 m5a moved doc to
//      dual-pair motif storage; the funnel writes both pairs regardless).
//      Passing m_project->motif preserves the funnel's API stability;
//      the value doesn't affect the result.
//   4. Mirrors active_doc->snap back into m_project->snap. The funnel
//      doesn't have a project pointer so it can't do this itself; this
//      is the same post-apply step ThemesPanel::on_apply_clicked's
//      do_apply lambda performs after walking its targets list. Required
//      for the snap state to round-trip through save (the Toolbar's
//      snap popover writes to m_project->snap as canonical; we have to
//      keep it in sync after a doc-side write).
//   5. Fires ThemeLibrary::signal_theme_applied(id). The library
//      doesn't actually mutate (apply touches the doc, not the
//      library), but the signal is the seam where MainWindow's
//      canvas-refresh cascade hooks in. See ThemeLibrary.hpp signals
//      block — the signal exists specifically to let drivers OTHER
//      than the panel (i.e. the script) hand off to the same refresh
//      callback the panel's set_on_changed installed.
//
// What it does NOT do:
//   * Push a command. apply is non-undoable in v1 by Theme.hpp design.
//     A scripted apply line is silently irreversible — Ctrl+Z after a
//     `themes.t apply` line walks past it as if it weren't there
//     (because there's no command to walk past). Script authors who
//     need apply to participate in an undo group will need to wait
//     for the (out-of-m1-scope) EditDocStateCommand option.
//   * Confirm with the user. The panel pops a modal warning the user
//     that apply isn't undoable; scripts skip this entirely (the
//     bareword `apply` IS the confirmation). The Scripter window is
//     the script author's domain; they own the consequences of every
//     line they run.
//   * Operate on multiple docs. v1 is active-doc-only. The verb
//     signature accepts no positional args today (or, in the
//     collection form, accepts exactly one — the theme id), leaving
//     room for a future `apply "<doc-spec>"` arg that the doc
//     Scriptable design will inform. The single-doc default is the
//     conservative landing point — if multi-doc lands later, scripts
//     written for v1 keep working with no edits.
//
// **capture** — collection-only `themes capture` / `themes capture
// "<name>"` / `themes capture "<name>" "<category>"`. There's no proxy
// form: capture CREATES a new theme, so the existing proxy's id isn't
// meaningful (same reason `new` is collection-only).
//
// What it does:
//   1. Refuses if no project or no active doc.
//   2. Calls capture_theme_from_doc(*active_doc, m_project->motif) —
//      same motif-arg-is-cosmetic story as apply.
//   3. Sets header.name from args[0] if provided. If args[0] is empty
//      or absent, auto-generates "Theme N" by walking has_user_name
//      from N=1 (mirror of ThemesPanel::on_save_current_as_theme's
//      proposal walk). Note: the panel's commit also has a
//      "name-collides → append N" loop for the case where the user
//      typed a duplicate — the script form skips that, since duplicate
//      names are not a model-level error and a script that wants
//      uniqueness can call find_by_name or check user_ids first.
//   4. Sets header.category from args[1] if provided; empty is
//      meaningful (= uncategorised). Default is empty.
//   5. Pushes AddThemeCommand. Library mints the fresh "thm_<uuid>";
//      we capture m_assigned_id and return it.
//
// Capture IS undoable (AddThemeCommand handles the undo by removing
// the captured theme). The captured value contains the active doc's
// settings AT CAPTURE TIME — subsequent doc edits don't ripple into
// the theme. This matches both the panel's behaviour and the
// theme-doesn't-bind invariant from Theme.hpp.
//
// **Why apply is proxy-AND-collection but capture is collection-only.**
//
// apply targets a SPECIFIC theme by id. The dotted form
// `themes.<id> apply` reads naturally ("apply this theme") and the
// collection form `themes apply "<id>"` reads as the by-id alternative
// for scripts that have an id in hand without wanting to materialise
// a proxy. Both shapes appear naturally in real script patterns.
//
// capture CREATES a new theme; there's no existing id to address. The
// only sensible form is `themes capture <name?> <category?>` — the
// collection knows the project, the active doc, and how to push the
// AddThemeCommand. A `themes.<id> capture` form would require the
// proxy to either overwrite the theme it addresses (not what
// capture_theme_from_doc does — it returns a fresh value-typed Theme)
// or ignore its own id (semantically incoherent). So capture is
// collection-only by design, not by oversight.
//
// **Refresh cascade.** Both verbs need the canvas to redraw after they
// fire, otherwise the operator sees stale state until the next click /
// inspector toggle. The two paths:
//
//   * apply → signal_theme_applied(id). MainWindow_zones wires this to
//     the same callback `m_themes.set_on_changed` installs (queue_draw +
//     notify_document_changed + refresh_inspector + schedule_save).
//
//   * capture → signal_theme_added(id) (existing). ThemesPanel already
//     listens on this for its library-list rebuild; MainWindow's
//     existing wiring of the panel's on_changed handles the schedule_save
//     side. No new signal needed for capture.
//
// **Eyes-half story.** Both verbs SHOULD update the canvas visibly:
//
//   * apply: the active doc's units / colours / guides / grid /
//     margins / snap all reflect the theme's values immediately after
//     the line runs. If grid/margin layers had to be created or
//     removed, the layer panel re-renders too.
//
//   * capture: the active doc looks identical (capture is pure read).
//     The visible change is in ThemesPanel — a new row appears with
//     the captured name (or auto-generated "Theme N"). Same eyes-half
//     check as `themes new`.
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
