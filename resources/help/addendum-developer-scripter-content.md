# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Content reference

The **collection-router** Scriptables — surfaces that address a
*set* of model objects (layers, swatches, styles, themes,
palettes, guides) and route per-instance verbs through a
transient proxy. Plus the panel-bound Scriptables that mirror
the Content area's panel widgets (`pnl_styles`).

Unlike Singletons (one registry entry, no routing) or the
Toolbar / Inspector substrate (no umbrella Scriptable, just
named widgets), every Scriptable on this page is **two surfaces
in one name**:

- The **collection** — addressed by the bare name (`layers`,
  `swatches`, `guides`, …). Holds collection-scope verbs (create,
  delete, enumerate) and collection-scope read-only properties
  (count, all_iids).
- The **per-instance proxy** — addressed by the dotted form
  (`layers.<iid>`, `swatches.<id>`, `guides.<iid>`). Materialised
  on demand by the collection's router hook, lives only until the
  end of the statement that addressed it. Holds the per-row
  verbs (rename, toggle, edit field) and per-row read-only
  properties (name, locked, visible, …).

This split exists because today's grammar can't apply a verb to
a *set* of objects in one step — there's no `foreach`, no list
type, no slice notation. The collection and proxy are the
shapes that fit the grammar's word-pair surface: a script
calls the collection to enumerate or create, then loops over
iids in its own driver, calling the proxy per row.

## Pages in this reference

This page covers six collection routers plus the panel-bound
Styles surface. Three are shipped here; three more land in the
next session:

- **`layers`** — every Type::Layer row in the active document.
- **`swatches`** — the project's swatch library (defaults +
  custom).
- **`guides`** — the active document's guide layer children
  (horizontal / vertical / arbitrary-angle alignment guides).
- **`styles`** — the project's named style library
  (fill / stroke / shadow bundles).
- **`themes`** — the doc-appearance catalogue (unit / motif /
  guides / grid / margins / snap).
- **`palettes`** — the palette catalogue (named ordered
  lists of swatch ids).
- **`pnl_styles`** — the Styles panel widget (UI-bound,
  distinct shape — derives from PanelScriptable, hosts
  walkthrough-narration verbs like `highlight_self`,
  `focus_section`).

This page now covers all seven Content-area Scriptables.

## Conventions on this page

- **`<iid>`** — a UUID-shaped identifier minted by Curvz when
  a model object is created. Returned from `new` verbs; passed
  into `delete` verbs and per-instance addressing.
- **`<id>`** — same shape, but Swatches uses `id` instead of
  `iid` in its property names because the swatch library's
  identifier type predates the iid convention. The two are
  interchangeable as UUID strings; the spelling difference is
  purely property-name nomenclature.
- **`<bool>`** — `true` or `false`, either as a quoted string
  or an unquoted token (the DSL accepts both).
- **`<number>`** — Int or Double literal.
- **`<string>`** — quoted string, single-line.

Comma-separated string returns (e.g. `all_iids`) reflect the
absence of a list type in `ScriptValue` — see the **Language
reference** for the rationale and the split-on-comma idiom that
fits the grammar's `result` slot.

**RunContext masks on this page.** None of the collection
routers below override `context_mask()` — every verb defaults
to **`all_three`** (TestRunner, Scripter, Macro). The
narrower-than-default masks live on the singletons (`proj` /
`export`) and on a small fraction of the action wrapper's
verbs (see **Header & menus reference**). The Content
collections all mutate ordinary document or library state that
doesn't destroy the script's readable surface — none of them
qualify for a Scripter mask carve-out, and none want one.

## ❶ `layers` — Layer rows in the active document

The script-addressable surface for the Layers panel. The
collection wraps every Type::Layer in the active document
(structural special-layers — Guide, Grid, Margin, Ref, Measure
— are NOT real layers in this sense and don't appear in
`layers`; their content surfaces are separate Scriptables —
e.g. `guides` below).

### Addressing

- **`layers`** — the collection (use this for `count` /
  `all_iids` / `new` / `delete` / `move` / `hide_others` /
  `show_all` / `find_by_name`).
- **`layers.<iid>`** — the per-row proxy (use this for
  `rename` / `set_visible` / `set_locked` / `color` /
  `opacity` / `activate`).

The proxy resolves the iid against the *current* document at
verb-dispatch time, not at addressing time — a layer deleted
between two lines of script invalidates the proxy on the
second line (verbs become no-op-returning-Null). Cross-doc
resolution works the same way: `delete <iid>` finds the doc
the iid actually lives in and operates there, not necessarily
the active doc.

### Collection-scope verbs (on `layers`)

- **`new`** — create a fresh Type::Layer in the active doc.
  No arguments today (name is auto-generated via
  `next_default_name`, colour is left empty so the panel falls
  back to "no colour tag" rendering). Inserts at the first
  position after any special-layer slots (matches what the +
  button in the Layers panel does). Returns the new iid as
  String; returns empty string if no project is loaded or no
  active document exists. Pushes `AddLayerCommand` (undoable).
- **`delete <iid>`** — remove the named layer from whichever
  document holds it. Refuses (returns Null without mutation)
  on any of: empty iid, iid not found in any doc, iid resolves
  to a non-Type::Layer node, deletion would leave zero real
  layers in the doc. Pushes `DeleteLayerCommand` (undoable).
  The "at least one real layer per doc" invariant matches the
  panel's guard.
- **`move <iid> <direction>`** — reorder the named layer.
  `<direction>` is one of `up` / `down` / `top` / `bottom`
  (string literal). Cross-doc resolution applies: a layer in
  a non-active doc reorders within its own doc. Active-layer
  index follows the moved layer if it WAS the active one;
  otherwise stays put. Pushes `ReorderLayersCommand`
  (undoable). Refuses on missing args, unknown iid, or
  malformed direction.
- **`hide_others <iid>`** — set `visible = false` on every
  real layer EXCEPT the named one. Pushes one
  `EditLayerFieldCommand` per layer that actually flipped
  (chatty for undo — each hide is its own step). No-op refuse
  if `<iid>` is empty. Already-hidden layers are skipped.
- **`show_all`** — set `visible = true` on every real layer
  in the active doc. Same per-layer command-push shape as
  `hide_others`. Already-visible layers are skipped.
- **`find_by_name <name>`** — return the iid of the first
  Type::Layer in the active doc with `name == <name>`, or
  empty string on miss. Read-shaped, but lives on `invoke`
  rather than `query` because the grammar's query path can't
  take arguments today. Returns through the `=` output line.

### Per-instance verbs (on `layers.<iid>`)

- **`toggle_visible`** — flip `visible`. No arguments. Pushes
  `EditLayerFieldCommand{Field::Visible}`.
- **`set_visible <bool>`** — write `visible`. No-op (no
  command) if already in the requested state. Pushes
  `EditLayerFieldCommand{Field::Visible}` on change.
- **`toggle_locked`** — flip `locked`. No arguments. Pushes
  `EditLayerFieldCommand{Field::Locked}`.
- **`set_locked <bool>`** — write `locked`. No-op if already
  in the requested state. Pushes
  `EditLayerFieldCommand{Field::Locked}` on change.
- **`rename <name>`** — write `name`. No-op if name is empty
  or unchanged. Pushes `EditLayerFieldCommand{Field::Name}`.
- **`color <hex>`** — write the layer's colour tag (the
  swatch shown next to the layer name in the panel). `<hex>`
  is a `#rrggbb` string or empty string for "no colour tag".
  Pushes `EditLayerFieldCommand{Field::Color}` on change. The
  `set_color` alias is accepted but deprecated — see the
  **Aliases** note below.
- **`opacity <0..1>`** — write `opacity`. Clamped to [0, 1]
  before storage. **No undo** — `EditLayerFieldCommand` has
  no `Field::Opacity` slot today, so the verb is direct-write
  on the model. The mutation is reversible by another
  `opacity` call but won't appear in the undo stack. The
  `set_opacity` alias is accepted but deprecated.
- **`activate`** — make this layer the active layer in its
  doc (sets `active_layer_index`). Walks the project to find
  which doc holds the iid, so activate can promote a layer
  in a non-active doc to that doc's active layer. **No undo**
  — `active_layer_index` isn't undoable for users either
  (the panel mutates it directly without pushing). The
  `make_active` alias is accepted but deprecated.

**Aliases.** Three pre-s218 names are still accepted by the
dispatcher: `set_color` (= `color`), `set_opacity` (=
`opacity`), `make_active` (= `activate`). They're listed as
deprecated in CANON and will be removed once the test corpus
sweeps; new scripts should use the canonical names above.

### Collection-scope properties (on `layers`)

- **`count`** — Int. Number of real (Type::Layer) layers in
  the active doc. Excludes guide/grid/margin/ref/measure
  layers. Returns 0 if no project loaded or no active doc.
- **`all_iids`** — String. Comma-separated iids of all real
  layers in the active doc, in panel order (index 0 first,
  which renders at the top of the Layers panel). Empty
  string if zero real layers.
- **`active_iid`** — String. The iid of the currently-active
  layer, or empty string if none.

### Per-instance properties (on `layers.<iid>`)

- **`name`** — String. The layer's display name.
- **`visible`** — Bool.
- **`locked`** — Bool.
- **`opacity`** — Double. In [0, 1].
- **`color`** — String. `#rrggbb` hex or empty (no colour
  tag).
- **`child_count`** — Int. Number of direct children
  (objects in this layer). Not recursive.
- **`iid`** — String. The same iid used to address the
  proxy. Useful when a script has the proxy in a `set` slot
  and needs the bare iid back.

### Examples

Create a layer, rename it, paint it blue, count the result:

```
set lid to (layers new)
layers.lid rename "Sketch"
layers.lid color "#3366ff"
get layers count
```

Round-trip an iid through `all_iids` and act on each:

```
set ids to (get layers all_iids)
list ids
# Output: ids = "<iid-1>,<iid-2>,<iid-3>"
# Driver script splits on comma, then per-iid:
layers.<each-iid> set_visible false
```

Hide every layer except a named one, find that named one
first:

```
set keep to (layers find_by_name "Reference photo")
layers hide_others keep
```

## ❷ `swatches` — the project's swatch library

The script-addressable surface for the swatch library that
lives on `CurvzProject::swatches`. Two tiers: **defaults**
(read-only — the built-in colour set every project starts
with) and **custom** (script- or panel-created, full mutation
surface). The collection's verbs and properties expose both
tiers; per-instance verbs refuse on defaults.

### Addressing

- **`swatches`** — the collection.
- **`swatches.<id>`** — the per-row proxy. `<id>` is the
  SwatchId returned by `new` / `duplicate` (or read from
  `all_ids` / `custom_ids` / `default_ids`).

**Naming note.** Swatch identifiers are spelt `id` in property
names (not `iid`) because the swatch library's identifier type
predates the iid convention. Format is identical (UUID
string); the spelling difference is purely nomenclature. Scripts
that hold an `id` and an `iid` in adjacent slots can compare
or pass them interchangeably — they're both `String`-shaped.

### Collection-scope verbs (on `swatches`)

- **`new [<name>] [<hex>]`** — create a new SolidSwatch in the
  custom tier. Both arguments optional. `<name>` defaults to
  empty string (the inspector falls back to auto-derived
  region name on display). `<hex>` is `#rrggbb` or `#rrggbbaa`;
  defaults to pure black. Parse failure on `<hex>` falls back
  to black (the swatch is still created — same silent-reject
  posture the proxy `color` verb uses for hex failures).
  Returns the minted id as String. Also adds the new swatch
  to the active palette so it appears in the panel grid
  (mirrors `SwatchesPanel::on_new_swatch`). Pushes
  `AddSwatchCommand` (undoable; library-side only — the
  palette membership is not snapshotted, see the s221 m1
  fix-1 note in CANON).
- **`delete <id>`** — remove the named swatch from the
  library. Refuses on any of: empty id, unknown id, id is
  defaults-tier. Pushes `RemoveSwatchCommand` (undoable). Does
  NOT touch object bindings (`fill_swatch_id` /
  `stroke_swatch_id` on bound SceneNodes) — that step lives on
  the panel handler, not in the Scriptable surface, by
  design. Scripts that need to clean bindings should do their
  own scene-walk.
- **`duplicate <id>`** — copy the named swatch into a fresh
  custom-tier swatch with `" copy"` appended to the name
  (empty name stays empty). Source can be from either tier —
  this is the "duplicate to edit" affordance for defaults
  swatches. Returns the minted id. Pushes `AddSwatchCommand`.
  Non-SolidSwatch sources (gradient / path-blend variants)
  return Null without mutation — not yet handled.
- **`rename <id> <name>`** — write the swatch's display
  name. Equivalent to `swatches.<id> rename <name>`; kept on
  the collection too because scripts that have the id in
  hand without wanting to materialise the proxy can call it
  flat. Same refusal paths as the proxy form (empty id,
  unknown id, defaults-tier, name unchanged). Pushes
  `EditSwatchCommand`.
- **`find_by_name <name>`** — return the id of the first
  swatch (any tier, defaults first) with `header.name ==
  <name>`, or empty string on miss. Same invoke-shaped read
  pattern as `layers find_by_name`. Names are not unique by
  construction; first-hit wins.

### Per-instance verbs (on `swatches.<id>`)

Every mutating verb refuses (returns Null without mutation)
on a defaults-tier id. The collection-level `duplicate` verb
is the affordance for editing a defaults swatch — duplicate
it into custom, then edit the copy.

- **`rename <name>`** — write the swatch's display name.
  Refuses if defaults-tier or name unchanged. Empty name is
  meaningful (clears the user-supplied name; inspector falls
  back to the auto-derived region name). Pushes
  `EditSwatchCommand` (carries before/after name with colour
  unchanged).
- **`color <hex>`** — write the swatch's colour. `<hex>` is
  `#rrggbb` or `#rrggbbaa`. Refuses if defaults-tier, hex is
  empty, hex fails to parse, the swatch is not a SolidSwatch
  (gradient / path-blend variants), or the colour is
  unchanged. Pushes `EditSwatchCommand` (carries
  before/after colour with name unchanged).

### Collection-scope properties (on `swatches`)

- **`count`** — Int. Total swatch count (defaults + custom).
- **`all_ids`** — String. Comma-separated ids of every
  swatch, defaults first then custom, insertion order within
  each tier.
- **`custom_ids`** — String. Comma-separated ids of
  custom-tier swatches only.
- **`default_ids`** — String. Comma-separated ids of
  defaults-tier swatches only.

### Per-instance properties (on `swatches.<id>`)

- **`name`** — String. The swatch's display name (may be
  empty for unnamed user swatches).
- **`color`** — String. `#rrggbb` hex for SolidSwatch
  variants; empty string for gradient / path-blend variants
  (symmetric with the proxy `color` verb's variant guard —
  read what you could write).
- **`is_default`** — Bool. True iff this swatch lives in the
  defaults tier.
- **`iid`** — String. The same id used to address the
  proxy. Spelt `iid` on the property name even though the
  arg-side spelling is `id` — minor inconsistency carried
  forward from the s217 m2 sweep; both forms refer to the
  same SwatchId.

### Examples

Create a custom swatch, edit its colour, look it up by name:

```
set sid to (swatches new "Sunset" "#ff6633")
swatches.sid color "#ff7744"
set found to (swatches find_by_name "Sunset")
get swatches.found color
```

Duplicate a defaults swatch into custom for editing:

```
set red_def to (swatches find_by_name "Red")
set red_copy to (swatches duplicate red_def)
swatches.red_copy rename "My red"
swatches.red_copy color "#cc2233"
```

Partition the library:

```
get swatches count
get swatches default_ids
get swatches custom_ids
```

## ❸ `guides` — alignment guides in the active document

The script-addressable surface for the **document-scope** guide
layer's children (the per-doc red horizontal / vertical /
arbitrary-angle alignment lines, not the project-scope guide
templates and not the canvas grid). Guides are direct-write —
they are not part of the undo stack (the inspector doesn't push
for guide edits either; mutations are immediate).

### Addressing

- **`guides`** — the collection.
- **`guides.<iid>`** — the per-row proxy.

The collection operates on the active doc's `guide_layer()`
(created on demand by `ensure_guide_layer()` when `new` runs).
Per-instance verbs resolve cross-doc, same shape as `layers`:
a guide iid in a non-active doc is still editable.

### Collection-scope verbs (on `guides`)

- **`new`** — create a new horizontal guide at canvas centre
  (x = canvas_width / 2, y = canvas_height / 2, angle = 0)
  in the active doc. Forces the guide layer's `visible` to
  true (mirrors the canvas ruler-drag path: creating a guide
  implies the user wants to see guides). Refuses if no
  project / no active doc / the guide layer is locked.
  Returns the new iid. **Not undoable.**
- **`delete <iid>`** — remove the named guide. Cross-doc
  resolution (walks every doc to find which one holds the
  iid). Refuses on empty iid, unknown iid, or iid that
  doesn't resolve to a guide-shaped node. **Not undoable.**
- **`color <hex>`** — write the doc-level guide colour
  (`guide_color_r/g/b`). Empty or unparseable hex is silently
  rejected (no-op). This is doc-scope, not per-guide — all
  guides in the doc share the same colour, matching the
  inspector's Color row in Object → Guides.

### Per-instance verbs (on `guides.<iid>`)

- **`toggle_locked`** — flip `locked`.
- **`set_locked <bool>`** — write `locked`. No-op on
  unchanged.
- **`x <number>`** — write `guide_x` (canvas pixels). Direct
  write — no redraw kick; the canvas-side `doc_changed`
  listener picks it up on the next structural verb (or call
  `sleep 16` to flush a frame).
- **`y <number>`** — write `guide_y` (canvas pixels).
- **`angle <number>`** — write `guide_angle` (degrees, model
  convention: 0 = horizontal, 90 = vertical). No
  normalization — callers that want a specific range wrap
  modulo themselves. The inspector's angle spinner does no
  normalization either; mirror that.

### Collection-scope properties (on `guides`)

- **`count`** — Int. Number of guide-shaped children in the
  active doc's guide layer. Returns 0 if no guide layer
  exists yet.
- **`all_iids`** — String. Comma-separated iids of every
  guide in the active doc's guide layer, layer order.
- **`color`** — String. The doc-level guide colour as
  `#rrggbb` hex.

### Per-instance properties (on `guides.<iid>`)

- **`locked`** — Bool.
- **`x`** — Double. Canvas pixels.
- **`y`** — Double. Canvas pixels.
- **`angle`** — Double. Degrees.
- **`type`** — String. One of `h` / `v` / `a` —
  horizontal, vertical, or arbitrary-angle. Same axis-aligned
  tests `SceneNode::guide_is_horizontal` /
  `_vertical` use; case-lowered for script-stable lookups.
- **`iid`** — String. The same iid used to address the
  proxy.

### Examples

Create a vertical guide at x=200, lock it:

```
set gid to (guides new)
guides.gid x 200
guides.gid angle 90
guides.gid set_locked true
```

Delete every guide in the doc:

```
set ids to (get guides all_iids)
# Driver splits on comma, then:
guides delete <each-iid>
```

Read the doc-scope guide colour, change it:

```
get guides color
guides color "#ff0099"
```

## ❹ `styles` — the project's style library

The script-addressable surface for the **named style** library
that lives on `CurvzProject::styles`. A style bundles a fill
Paint, a stroke (paint + width + cap + join + miter limit),
and a shadow (enabled + offsets + blur + colour + opacity)
under a name and category. Two tiers: **app** (read-only —
built-in styles every project starts with, ids prefixed
`app:`) and **user** (script- or panel-created, full
mutation surface). The collection's verbs and properties
expose both tiers; per-instance mutating verbs refuse on
app-tier ids.

### Addressing

- **`styles`** — the collection.
- **`styles.<id>`** — the per-row proxy. `<id>` is a
  `stl_<uuid>` (user tier) or `app:<slug>` (app tier)
  string returned by `new` / `duplicate` or read from
  `all_ids` / `user_ids` / `app_ids`.

### Paint spec format

Six verbs and properties on this page (`fill`, `stroke_paint`,
and `shadow_color` on the proxy) take or return a **paint
spec** — a discriminating string the Scriptable's encoder /
decoder round-trip through. The format:

- **`none`** — the `None` Paint variant (no fill / no
  stroke paint).
- **`currentcolor`** — the `CurrentColor` variant (inherits
  the parent's currentColor in SVG terms).
- **`#rrggbb`** or **`#rrggbbaa`** — the `Solid` variant
  (8-bit RGB or RGBA). `#rgb` / `#rgba` shorthand also
  accepted on the way in.
- **`swatch:<id>`** — the `SwatchRef` variant referencing
  a swatch in the project's swatch library. The library is
  consulted for the fallback colour at set time; if the
  swatch later moves or is deleted, the ref's cached fallback
  renders (matches the panel's "dead ref" degradation path).

Gradients (the `Gradient` variant) are **not addressable**
through this surface — the encoder returns empty string for
them, and the decoder doesn't have a gradient spec syntax.
Reading a style with a gradient fill returns `""`; writing
the empty string back is rejected. Scripts that need gradient
editing work through the inspector picker today; a future
gradient spec syntax is on the roadmap.

### Line cap / line join vocabulary

Two stroke verbs (`stroke_cap`, `stroke_join`) take enum
strings. Unknown vocabulary is silent no-op (matches the
paint spec posture for malformed input). The accepted
strings, lowercase:

- **`stroke_cap`** — `butt` / `round` / `square`.
- **`stroke_join`** — `miter` / `round` / `bevel`.

### Collection-scope verbs (on `styles`)

- **`new [<name>] [<category>]`** — create a new
  user-tier style. Both arguments optional. Seed is a
  default-constructed Style (`None` fill paint, default
  StrokeAppearance, shadow disabled) — same as what the +
  button in the panel produces. Empty name is allowed (the
  panel falls back to displaying `header.id`); empty
  category lands in the "(uncategorised)" bucket. Returns
  the minted `stl_<uuid>` id. Pushes `AddStyleCommand`
  (undoable). Also navigates the panel's active category to
  the new style's category so the user sees the result
  (mirrors `StylesPanel::action_create_empty`).
- **`delete <id>`** — remove the named style. Refuses on
  empty id, unknown id, or app-tier id. Pushes
  `RemoveStyleCommand` (undoable, carries the full Style
  snapshot for round-trip).
- **`duplicate <id>`** — copy the named style into a fresh
  user-tier style with `" copy"` appended (empty name stays
  empty). Source can be from either tier — duplicating an
  app style is the "duplicate to edit" affordance. Category
  is preserved verbatim (an app→user duplicate keeps the
  source's category, including the "Built-in" tier
  discriminant — matches `StylesPanel::action_duplicate`).
  Returns the new id. Pushes `AddStyleCommand`.
- **`rename <id> <name>`** — write the style's name. Same
  as `styles.<id> rename <name>`; collection form provided
  for scripts that have the id flat. Refuses on empty id,
  app-tier id, unknown id, or unchanged name. Pushes
  `UpdateStyleCommand`.
- **`category <id> <category>`** — write the style's
  category. Two-arg form mirrors `rename`. Same refusal
  paths; empty category is meaningful (lands in
  "(uncategorised)"). Pushes `UpdateStyleCommand`.
- **`find_by_name <name>`** — return the id of the first
  style (any tier) with `header.name == <name>`, empty
  string on miss. Iteration order: app tier first (in
  `app_categories()` order, each category in insertion
  order), then user tier. Same invoke-shaped read pattern
  as elsewhere.

### Per-instance verbs (on `styles.<id>`)

Every mutating verb refuses (returns Null without mutation)
on an app-tier id. The collection's `duplicate` verb is the
affordance for editing an app style.

- **`rename <name>`** — write `header.name`. Empty is a
  real value here (clears the name) — the panel's
  cancel-as-empty is a modal UX choice, not a model rule.
  No-op on unchanged. Pushes `UpdateStyleCommand`.
- **`category <category>`** — write `header.category`.
  Empty lands the style in "(uncategorised)". Pushes
  `UpdateStyleCommand`.
- **`fill <paint-spec>`** — write the fill Paint. See
  paint-spec format above. Malformed / gradient specs are
  silent no-op. Pushes `UpdateStyleCommand`.
- **`stroke_paint <paint-spec>`** — write the stroke
  Paint. Same parsing as `fill`.
- **`stroke_width <number>`** — write `stroke.width`
  (canvas units). **No clamp** — inspector clamps at the
  UI layer, the model accepts whatever script supplies.
- **`stroke_cap <cap>`** — `butt` / `round` / `square`.
- **`stroke_join <join>`** — `miter` / `round` / `bevel`.
- **`stroke_miter_limit <number>`** — write
  `stroke.miter_limit`. No clamp.
- **`shadow_enabled <bool>`** — toggle the drop shadow.
- **`shadow_dx <number>`** — shadow x offset (canvas
  units).
- **`shadow_dy <number>`** — shadow y offset.
- **`shadow_blur <number>`** — shadow blur radius.
- **`shadow_color <hex>`** — shadow colour. `<hex>` is
  `#rrggbb` or `#rrggbbaa`. Note this is the RGBA bundle
  (`color_r/g/b/a`); `shadow_opacity` is a separate dial.
  Parse failure silent no-op.
- **`shadow_opacity <number>`** — shadow opacity dial.
  Distinct from `shadow_color`'s alpha channel — the model
  multiplies both at render time, matching the Inspector's
  Shadow section.

All thirteen field setters push a single `UpdateStyleCommand`
with a full before/after Style snapshot. No-op writes (value
unchanged) skip the push.

### Collection-scope properties (on `styles`)

- **`count`** — Int. Total style count (`app_style_count +
  user_style_count`).
- **`all_ids`** — String. Comma-separated, app tier first.
- **`user_ids`** — String. User-tier ids only.
- **`app_ids`** — String. App-tier ids only.

### Per-instance properties (on `styles.<id>`)

- **`name`** — String.
- **`category`** — String. Empty string for uncategorised.
- **`is_built_in`** — Bool. Cheap prefix check on the id.
- **`iid`** — String. The same id used to address the
  proxy.
- **`fill`** — String. Paint spec (see format above).
  Empty for gradient fills.
- **`stroke_paint`** — String. Paint spec.
- **`stroke_width`** — Double.
- **`stroke_cap`** — String. `butt` / `round` / `square`.
- **`stroke_join`** — String. `miter` / `round` / `bevel`.
- **`stroke_miter_limit`** — Double.
- **`shadow_enabled`** — Bool.
- **`shadow_dx`** — Double.
- **`shadow_dy`** — Double.
- **`shadow_blur`** — Double.
- **`shadow_color`** — String. `#rrggbb[aa]` hex form.
- **`shadow_opacity`** — Double.

### Examples

Create a custom style with a red fill and a dotted shadow:

```
set sid to (styles new "Card outline" "UI")
styles.sid fill "#cc2233"
styles.sid stroke_paint "none"
styles.sid shadow_enabled true
styles.sid shadow_dx 2
styles.sid shadow_dy 2
styles.sid shadow_blur 6
styles.sid shadow_color "#00000044"
```

Reference a swatch as the fill paint:

```
set red to (swatches find_by_name "Red")
set sid to (styles new "Red badge")
styles.sid fill "swatch:" + red
```
*(Note: today's grammar has no string-concat operator — the
example assumes a future addition. Until then, scripts that
need swatch refs compose the spec string outside the
Scripter and inject via `set`.)*

Duplicate an app style into user-tier for editing:

```
set src to (styles find_by_name "Outline")
set copy to (styles duplicate src)
styles.copy rename "Heavy outline"
styles.copy stroke_width 4
```

## ❺ `themes` — the theme catalogue

The script-addressable surface for the **theme** library on
`CurvzProject::themes`. A theme bundles every doc-scope
appearance setting (unit, motif colours, guide colour, grid
spacing / colour / visibility, margin geometry, snap toggles)
into a named, applicable record. Same two-tier story as
Styles: **app** (read-only built-in themes) and **user**
(script- or panel-created). v1 ships with no app themes,
making the app-tier branch academic today — but the guards
and `app_ids` surface are in place.

### Addressing

- **`themes`** — the collection.
- **`themes.<id>`** — the per-row proxy. `<id>` is a
  `thm_<uuid>` (user tier) or `app:<slug>` (app tier —
  none exist in v1, but the prefix is reserved).

### Apply vs. capture

Two verbs on this Scriptable don't fit the create / read /
edit / delete pattern of the other collections, and they're
the reason themes exist as a Scriptable surface at all:

- **`apply`** — read a theme's record and write its fields
  onto the active document. **Not undoable.** Both proxy
  (`themes.<id> apply`) and collection (`themes apply <id>`)
  forms are supported. Apply runs even on the app tier
  (read-only on the *theme*; the *doc* is the mutation
  target).
- **`capture`** — read the active document's appearance
  fields and create a new user-tier theme from them.
  Collection-only (`themes capture` / `themes capture
  "<name>"` / `themes capture "<name>" "<category>"`).
  Undoable — Ctrl+Z removes the captured theme.

These two are the script equivalent of the panel's "Apply"
button and the "Save current as theme…" entry. The proxy
form of `apply` lives above the app-tier mutation guard, so
applying a built-in theme works — it doesn't mutate the
theme, only the doc.

### Field vocabulary

The proxy carries 34 per-field setters and reads, grouped
into six sub-bundles. Unknown enum strings are silent no-op
(matches the Styles paint-spec posture); unchanged values
skip the command push.

- **Unit** — `unit` takes `px` / `in` / `mm` / `pt`. The
  model field is `units.display_unit`; the verb drops the
  `display_` prefix the same way `category` drops `header.`.
- **Motif** — six fields, dark/light × artboard/workspace/creation.
  All `#rrggbb` hex (no alpha — the motif colours are
  RGB-only in the model; the hex parser tolerates alpha on
  the way in but it's discarded on write, and reads always
  emit the no-alpha form).
- **Guides** — `guide_color` (hex), `guide_visible` (bool).
- **Grid** — `grid_enabled` / `grid_visible` (bool),
  `grid_spacing_x` / `grid_spacing_y` / `grid_offset_x` /
  `grid_offset_y` (number, canvas units), `grid_color`
  (hex), `grid_dots` (bool — switches grid rendering from
  lines to dots).
- **Margins** — `margin_enabled` / `margin_visible` (bool),
  `margin_top` / `margin_bottom` / `margin_left` /
  `margin_right` (number), `margin_columns` / `margin_rows`
  (int count), `margin_col_gap` / `margin_row_gap` (number),
  `margin_color` (hex).
- **Snap** — `snap_enabled` (master toggle), plus six
  category bools: `snap_guides` / `snap_grid` /
  `snap_margins` / `snap_nodes` / `snap_edges` /
  `snap_centers`.

All 34 field setters push a single `UpdateThemeCommand`
with a full before/after Theme snapshot. Each property is
readable through the proxy under the same name as its
setter verb.

### Collection-scope verbs (on `themes`)

- **`new [<name>] [<category>]`** — create a new user-tier
  theme with default-constructed fields. Both arguments
  optional. Returns the minted `thm_<uuid>` id. Pushes
  `AddThemeCommand`.
- **`delete <id>`** — remove the named theme. Refuses on
  app-tier ids. Pushes `RemoveThemeCommand` (carries the
  full Theme snapshot).
- **`duplicate <id>`** — copy into a fresh user-tier theme
  with `" copy"` appended. Returns the new id. Pushes
  `AddThemeCommand`.
- **`rename <id> <name>`** — same shape as `styles
  rename`. Refuses on app-tier or unchanged name.
- **`category <id> <category>`** — same shape as `styles
  category`. Note: today's ThemesPanel doesn't render a
  category-grouped view (themes are flat in the panel), so
  category is metadata-only from the UI's perspective. The
  verb is in place for parity with Styles and forward-compat
  with a future grouped panel.
- **`find_by_name <name>`** — invoke-shaped read; same
  iteration order as `styles find_by_name` (app tier first,
  then user tier).
- **`apply <id>`** — apply the named theme to the active
  document. Same as `themes.<id> apply`. Not undoable.
- **`capture [<name>] [<category>]`** — capture the active
  doc's appearance into a new user-tier theme. Both
  arguments optional. If `<name>` is missing or empty, the
  library walks `Theme N` from N=1 looking for an unused
  name (mirrors `ThemesPanel::on_save_current_as_theme`'s
  proposal walk; caps at 10000 attempts). Returns the new
  id. Pushes `AddThemeCommand` — capture IS undoable, even
  though `apply` isn't. (Apply mutates the doc; capture
  mutates the library. The library half rolls back cleanly,
  the doc half can't be expressed as a command without a
  full doc-state snapshot.)

### Per-instance verbs (on `themes.<id>`)

- **`apply`** — see above. Lives above the app-tier guard
  (works on built-in themes).
- **`rename <name>`** — write `header.name`.
- **`category <category>`** — write `header.category`.
- All 34 field setters from the **Field vocabulary** block
  above.

### Collection-scope properties (on `themes`)

- **`count`** — Int.
- **`all_ids`** — String. Comma-separated, app first.
- **`user_ids`** — String. User-tier only.
- **`app_ids`** — String. App-tier only (empty in v1).

### Per-instance properties (on `themes.<id>`)

- **`name`** — String.
- **`category`** — String.
- **`is_built_in`** — Bool.
- **`iid`** — String.
- All 34 field reads from the **Field vocabulary** block,
  one per setter, returning the matching primitive type
  (Bool / Int / Double / hex String).

### Examples

Capture the current doc into a named theme:

```
set tid to (themes capture "Pastel sketch" "Reference")
```

Apply a theme to whatever doc is active:

```
set tid to (themes find_by_name "Print proof")
themes apply tid
```

Edit a theme's grid spacing in user tier:

```
set tid to (themes find_by_name "My theme")
themes.tid grid_spacing_x 20
themes.tid grid_spacing_y 20
themes.tid grid_dots true
```

Round-trip a single motif field:

```
themes.tid motif_dark_workspace "#1a1a22"
get themes.tid motif_dark_workspace
# Output: themes.tid motif_dark_workspace = "#1a1a22"
```

## ❻ `palettes` — the palette catalogue

The script-addressable surface for the **palette** library on
`CurvzProject::swatches` (palettes share the SwatchLibrary
data structure with swatches — the panel renders palettes as
named collections of swatch chips). A palette is a *named
ordered list of swatch ids*; the swatches themselves live
under `swatches.*`. Two tiers: **defaults** (built-in palettes
every project starts with) and **custom** (script- or
panel-created).

The library tracks an **active palette** — the one whose
swatches show in the panel's grid. The `swatches new` and
`swatches duplicate` verbs add their result to whatever the
active palette is at the time (see the Swatches section
above). `palettes activate` is the verb that switches it.

### Addressing

- **`palettes`** — the collection.
- **`palettes.<iid>`** — the per-row proxy.

(Palettes use `iid` as the proxy-side spelling — same
inconsistency Swatches has, carried forward from the s243 m1
sweep. Palette iids are UUID strings; either spelling works
in argument slots.)

### What this Scriptable does NOT do

Palettes are *containers of swatch ids*, but there's no
script verb today to add or remove a swatch from a palette
(or to reorder its contents). Those operations exist on the
`SwatchLibrary` C++ side (`add_to_palette`,
`remove_from_palette`), but they aren't exposed through this
Scriptable — partly because palette membership isn't fully
undoable yet (the s221 m1 fix-1 note in CANON tracks this),
partly because the membership-mutation surface needs design
work the panel hasn't shipped either. The verbs below cover
palette **identity** (create / delete / duplicate / rename /
activate) but not palette **content**.

Reading palette content works — the proxy's `swatches`
property returns the ordered swatch-id list. Scripts that
need to drive membership today do so through the
`swatches new` path (which adds to the active palette as a
side effect).

### Collection-scope verbs (on `palettes`)

- **`new <name>`** — create a new custom-tier palette with
  the given name. **Name is required** (unlike `styles new`
  and `themes new`, which accept empty names — palettes
  need names; the panel's UX matches this). Empty name
  returns `""` without pushing. Returns the minted iid.
  Pushes `AddPaletteCommand`. The new palette is made active
  (mirrors `SwatchesPanel::on_new_palette`'s UX) and the
  command's `set_make_active(true)` records the activation
  on the command so redo restores it.
- **`delete <iid>`** — remove the named palette. Refuses
  on empty iid, unknown iid, or defaults-tier iid. Pushes
  `RemovePaletteCommand` (carries the palette value AND the
  active-state pre-remove for round-trip undo).
- **`duplicate <iid> [<new_name>]`** — copy the named
  palette into a custom-tier palette. `<new_name>` is
  optional — empty falls through to
  `SwatchLibrary::duplicate_palette`'s "append ' copy' to
  source name" default. Source can be from either tier
  (duplicate-to-edit affordance for defaults palettes).
  Returns the new iid. The duplicate becomes active.
  Wrapped in `AddPaletteCommand::already_added` so the
  library's actual mutation happens once (in
  `duplicate_palette`) and the command captures the result
  for undo.
- **`rename <iid> <name>`** — write the palette name. Same
  shape as the proxy rename; refuses on defaults-tier,
  empty iid, empty name, unknown iid, or unchanged name.
  Pushes `RenamePaletteCommand`.
- **`activate <iid>`** — set the library's active palette.
  **Not undoable** — active-palette is transient working
  state. Works on either tier (activation isn't a palette
  mutation). Refuses on empty / unknown iid (avoids
  destructive silent clear on a typo).
- **`find_by_name <name>`** — invoke-shaped read. Returns
  the iid of the first palette (any tier, defaults first)
  with name match. Empty string on miss.

### Per-instance verbs (on `palettes.<iid>`)

- **`rename <name>`** — write the palette name. Refuses on
  defaults-tier, empty name, unchanged name. Empty name is
  a cancel here (unlike `styles rename`'s "empty is a real
  value" — palettes require names by the panel's UX).
  Pushes `RenamePaletteCommand`.
- **`activate`** — set this palette as the library's
  active palette. Not undoable. No-arg (the iid is the
  proxy's iid).

### Collection-scope properties (on `palettes`)

- **`count`** — Int.
- **`all_ids`** — String. Comma-separated, defaults
  first.
- **`custom_ids`** — String. Custom-tier only.
- **`default_ids`** — String. Defaults-tier only.
- **`active_id`** — String. The iid of the currently-
  active palette, or empty string if none.

### Per-instance properties (on `palettes.<iid>`)

- **`name`** — String.
- **`is_default`** — Bool.
- **`is_active`** — Bool. True iff this palette's iid
  matches `lib.active_palette()`.
- **`swatch_count`** — Int. Number of swatch ids in the
  palette's ordered list.
- **`swatches`** — String. Comma-separated swatch ids in
  palette order. Empty string for an empty palette.
- **`iid`** — String.

### Examples

Create a palette, make it active, add two swatches into it:

```
set pid to (palettes new "Warm autumn")
swatches new "Pumpkin" "#cc6633"
swatches new "Mustard" "#ddaa22"
get palettes.pid swatches
# Output: palettes.pid swatches = "<sw-id-1>,<sw-id-2>"
```

(The two `swatches new` calls land in the active palette
because `palettes new` left this palette active. To add
swatches to a *different* palette, `palettes activate
<other-pid>` first.)

Switch the active palette to read its contents:

```
set pid to (palettes find_by_name "Defaults")
palettes activate pid
get palettes.pid swatch_count
```

Duplicate a defaults palette into custom for editing:

```
set src to (palettes find_by_name "Material")
set copy to (palettes duplicate src)
palettes.copy rename "Material trimmed"
```

## ❼ `pnl_styles` — the Styles panel widget

The script-addressable surface for the **Styles panel widget**
itself — not the style library (that's `❹ styles` above), but
the GTK panel that renders style chips, runs the category
dropdown, and hosts the kebab menu's `+ New style` action.

Unlike the six collection routers, `pnl_styles` is a
`PanelScriptable` — bound to a single GTK panel widget rather
than a model collection. The verbs are visual-narration moves
(highlight this widget, pop the kebab, expand my inspector
section) more than data-mutation operations. The intended
caller is a tutorial or macro-walkthrough script that wants
to direct the viewer's eye through panel UI.

Of the six Content-area panels (Styles, Themes, Palettes,
Layers, Swatches, Guides), only **Styles** has a
`PanelScriptable` surface in v1. The other panels accept
script-driven mutations through their model collections
(`themes apply`, `layers new`, etc.) but don't expose
walkthrough-narration verbs. If a future session adds a
ThemesPanelScriptable or similar, it would mirror this
section's shape.

### Addressing

- **`pnl_styles`** — the singleton registry entry. No
  per-instance proxy; the panel is unique within an app
  session.

### What pnl_styles lets scripts do

Four kinds of moves:

- **Headline navigation** — switch the category dropdown
  (`expand_category`).
- **Visual highlight** — pulse a CSS class for ~600ms on
  the panel itself or a specific kebab menu item, so the
  viewer's eye lands where the narrator's script is
  pointing (`highlight_self`, `highlight_menuitem`).
- **Kebab choreography** — pop and close the `+` kebab
  menu programmatically, for walkthroughs that demonstrate
  the menu (`show_kebab`, `hide_kebab`).
- **Inspector section orchestration** — expand or collapse
  the panel's own inspector section, or perform the
  composite "focus" move that collapses every other section
  and walks open the section chain leading to mine
  (`expand_section`, `collapse_section`, `focus_section`).

Plus one action shortcut that bypasses the kebab menu:
**`new_style`** invokes the same path the kebab's `+ New
style` entry does, useful for tests that don't want to
choreograph the popover.

### Verbs

- **`new_style`** — call the panel's `action_create_empty`
  handler directly. Same outcome as a user clicking
  `+ New style` from the kebab menu, but with no popover
  dance. Pushes `AddStyleCommand` (undoable; same command
  the kebab path uses). Returns Null.
- **`expand_category <name>`** — set the category dropdown
  to `<name>`. Resolution walks the panel's
  `m_category_order` (rebuilt on every refresh) to find a
  matching `(name, is_app_tier)` entry. **No-op on miss**
  — the script's intent is "go there if it exists" rather
  than "land somewhere"; silence is better than a stale
  fallback. Verify success by reading
  `active_category` post-call.
- **`highlight_self [<ms>]`** — pulse the `.demo-highlight`
  CSS class on the panel widget itself for `<ms>`
  milliseconds. Default duration 600ms. Negative or zero
  `<ms>` falls back to default (defensive against hostile
  inputs). Subsequent `highlight_*` calls cancel any
  in-flight pulse and start fresh — last call wins.
- **`show_kebab`** — pop up the kebab `+` MenuButton's
  popover. Idempotent. The model-button children of the
  popover construct after `popup()` returns — scripts that
  want to highlight a specific item with
  `highlight_menuitem` should let the idle loop run once
  (the verb handles the idle dance internally; the
  canonical prologue also adds a `sleep 400` for narration
  pacing).
- **`highlight_menuitem <suffix>`** — pulse the
  `.demo-highlight` class on the kebab menu item whose
  action name is `styles.<suffix>` (e.g. `<suffix>` =
  `create-empty`). Walks the popover widget tree looking
  for a `GtkModelButton` with the matching `action-name`
  property. Silently no-ops if the popover isn't open or
  the item isn't found. 600ms pulse, same default as
  `highlight_self`.
- **`hide_kebab`** — pop down the kebab popover. Cancels
  any in-flight `highlight_menuitem` pulse first (the
  popover destroys its model-button children on close, so
  the highlight timeout would dereference a dead widget
  otherwise). Cheap no-op if the popover is already closed.
- **`expand_section`** — invoke the panel's
  `m_section_apply(true)` closure (wired by MainWindow at
  panel construction). Opens the inspector section the
  panel lives in. Safe no-op if the closure is unset
  (panel running outside an inspector section, or
  MainWindow wiring forgotten — narrated scripts that hit
  this just don't get the pre-expand benefit).
- **`collapse_section`** — invoke `m_section_apply(false)`.
  Closes the panel's inspector section. Same no-op posture.
- **`focus_section`** — composite move: collapse every
  inspector section in MainWindow's registry, then walk
  the section chain (the list of section ids that lead to
  this panel) open. Invokes the focus closure MainWindow
  installed via `set_section_chain`. Safe no-op if the
  closure isn't wired. The headline visual-narration move
  for tutorial scripts: one verb takes the inspector from
  any state to "the panel I want the viewer to see is
  visible, everything else is collapsed."

The base class's `highlight_widget` and `cancel_highlight`
are **C++ protected helpers**, not script-callable verbs.
Scripts reach the highlight machinery through the panel-
specific verbs above (`highlight_self`, `highlight_menuitem`)
which call the helpers internally.

### Properties

- **`active_category`** — String. The category name
  currently selected in the panel's dropdown.
- **`active_is_app_tier`** — Bool. True iff the active
  category is from the app tier (Built-in). Distinguishes
  between user-tier and app-tier categories that share a
  display name (`m_category_order` carries the tier
  discriminant alongside the name — yes, "Built-in" is a
  distinct bucket per tier).
- **`kebab_visible`** — Bool. True iff the kebab popover
  is currently popped up. Reads through the MenuButton's
  `:active` GTK property — the canonical "is the popover
  open" query.
- **`section_open`** — Bool. True iff the inspector
  section the panel lives in is currently expanded. Reads
  the `shared_ptr<bool>` MainWindow installed via
  `set_section_state`. Returns false when the flag pointer
  is null (panel running outside an inspector section —
  narration can still query, just gets a stable false).

### Examples

Narrate the panel's "create a new style" affordance step by
step:

```
pnl_styles focus_section
pnl_styles highlight_self
sleep 600
pnl_styles show_kebab
sleep 400
pnl_styles highlight_menuitem "create-empty"
sleep 600
pnl_styles hide_kebab
pnl_styles new_style
```

Switch the dropdown to a known category and verify:

```
pnl_styles expand_category "UI"
get pnl_styles active_category
# Output: pnl_styles active_category = "UI"
get pnl_styles active_is_app_tier
# Output: pnl_styles active_is_app_tier = false
```

Walk the inspector to make the panel visible without
running a full focus narration:

```
pnl_styles expand_section
get pnl_styles section_open
# Output: pnl_styles section_open = true
```

## Reference complete

This page now covers all seven Content-area Scriptables: the
six collection routers (`layers`, `swatches`, `guides`,
`styles`, `themes`, `palettes`) and the one panel widget
(`pnl_styles`). The **Canvas & objects** reference covers
the on-canvas object surface; **Dialogs & popovers** covers
the modal-dialog widget catalogue. Both shipped in s284 —
the eight-leaf Scripter reference set is complete.


