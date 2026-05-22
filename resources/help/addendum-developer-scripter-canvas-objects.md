# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Canvas & objects reference

The **scene-content** Scriptable — the surface that addresses
everything a user would call a *thing on the canvas*: paths,
groups, compounds, clip groups, blends, warps, text, images,
reference markers, measurement markers. Plus a short trailer
on the canvas widget itself, which is **not** a Scriptable but
does carry two `set_name` substrate widgets that show up in
the Scripter's widget-name resolver.

## What this page covers

- **`objects`** — the collection router over every in-scope
  scene node in the active document, addressed by the bare
  name. Holds two set-scope reads (`count`, `all_iids`), two
  invoke-shaped reads (`find_by_name`, `find_by_type`), and
  the four structural verbs (`new`, `delete`, `group`,
  `ungroup`).
- **`objects.<iid>`** — the per-instance proxy materialised on
  demand by the collection router. Holds the 17-property read
  surface (identity / tree position / geometry / appearance)
  and the 16 mutating verbs (toggle / set / rename /
  set_path_data / set_fill / set_stroke / set_stroke_width /
  set_stroke_cap / set_stroke_join / set_stroke_miter /
  set_stroke_opacity / move / reparent / duplicate).
- **Canvas substrate widgets** — the two `set_name` ids on the
  canvas drawing area and the text-tool overlay entry. No
  Scriptable; addressable through the widget-name resolver
  the same way Toolbar buttons and Inspector spinners are.

## Conventions on this page

Same conventions as the rest of the reference: `<iid>` is a
UUID minted by Curvz; `<bool>` is `true`/`false`; `<number>`
is an Int or Double literal; `<string>` is a quoted string.
The `<hex>` token (used by `set_fill` / `set_stroke`) is a
`#rrggbb` or `#rrggbbaa` string. The `<d-string>` token (used
by `set_path_data`) is an SVG-d path string in **user space**
(see the user-space note below).

Comma-separated string returns (`all_iids`) reflect the
absence of a list type in `ScriptValue` — see the **Language
reference** for the split-on-comma idiom.

**RunContext masks on this page.** None of the verbs override
`context_mask()` — every verb defaults to **`all_three`**
(TestRunner, Scripter, Macro). The narrower masks live on the
singletons (`proj` / `export`), not here. Every verb on
`objects` mutates ordinary document state that doesn't
destroy the script's readable surface.

## ❶ `objects` — scene content in the active document

The script-addressable surface for everything on the canvas
that a user would interact with. Walks the whole document
tree (every layer's children, recursively), not just the
active layer. Cross-layer scripts work without a layer-
switching dance.

**Type scope.** The collection covers the ten scene-content
types: `path`, `group`, `compound`, `clipgroup`, `blend`,
`warp`, `text`, `image`, `ref`, `measurement`. The five
*structural* types — `layer`, `guide`, `guidelayer`,
`reflayer`, `measurelayer`, `gridlayer`, `marginlayer` — are
explicitly **out of scope** here; they have their own
Scriptables (`layers` for layers, `guides` for guides) or no
script addressability at this tier (the special structural
containers are doc-level properties, not row-bound surfaces).

Ref and Measurement leaves ARE in scope despite their parents
being special layers — a Ref or Measurement is a concrete
leaf with stable identity, and a script wanting to read its
properties does so via `objects.<iid>`. Their parent
containers belong to future Scriptables.

### Addressing

- **`objects`** — the collection (use this for `count` /
  `all_iids` / `find_by_name` / `find_by_type` / `new` /
  `delete` / `group` / `ungroup`).
- **`objects.<iid>`** — the per-row proxy (use this for every
  per-node read and every mutating verb).

The proxy resolves the iid against the *current* project at
verb-dispatch time, not at addressing time — a node deleted
between two lines of script invalidates the proxy on the
second line (verbs become no-op-returning-Null, reads return
empty / 0). Cross-doc resolution applies the same way `layers`
does it: a `get objects.<iid> name` finds the iid in whichever
doc holds it, not necessarily the active one. The iid is the
address; the doc is just where it lives.

**Active-doc vs project-wide scope.** Two surfaces follow
different rules:

- The **collection-scope reads** (`count`, `all_iids`,
  `find_by_name`, `find_by_type`) operate on the **active
  doc** only. `objects count` does not count nodes in
  background docs.
- The **proxy reads** and **proxy verbs** resolve iids
  **project-wide**. A proxy verb on a background-doc node
  mutates that doc, with the canvas-refresh callback firing
  on whatever doc the active canvas is rendering (so a
  mutation in a non-visible doc is invisible until the user
  switches to it).
- The **`new` verb** creates into the **active doc's active
  layer** specifically. Scripts that want to add into a
  non-active layer must call `layers.<lid> activate` first.
- The **`delete` / `group` / `ungroup` verbs** resolve iids
  project-wide, the same as the proxy verbs.

### Collection-scope verbs (on `objects`)

- **`new <type>`** — create a fresh scene object of the given
  type and insert it at the front of the active layer's
  children (front = visually on top, matching the canvas
  convention). Returns the new node's iid as String; binds to
  `result` so `set <var> to result` captures it for immediate
  addressing. Pushes `AddNodeCommand` (undoable). `<type>`
  vocabulary today: `path` (a Path with empty PathData) and
  `group` (an empty Group). Returns empty string on missing
  args, unknown type token, no active doc, or no active
  layer. **Name policy: newly-minted objects start with an
  empty `name`** — the empty-string sentinel that means "the
  user hasn't named me yet." Different from canvas-tool
  creation which auto-assigns `next_default_name` (e.g.
  `Path 1`). Use `rename` to give the new node a name.

- **`delete <iid>`** — remove the named scene object from its
  parent container, wherever in the project tree it lives.
  Pushes `DeleteObjectCommand` (undoable). Refuses (returns
  Null without mutation) on: empty iid, iid not resolvable,
  iid resolves to an out-of-scope type (layer / guide /
  special layer), the resolved node has no findable parent,
  or the node is owned by a non-children slot (clip_shape,
  blend_source_a, blend_source_b, warp_source — those are
  structural inputs to their containers, not free siblings).
  Scrubs the global undo stack for raw-pointer-capture
  references to the about-to-be-deleted node and its
  descendants — same defensive walk `layers delete` uses.

- **`group <iid1> <iid2> [<iid3> ...]`** — wrap two or more
  in-scope scene objects in a new Group node. The targets
  must be **siblings** (direct children of the same parent —
  a Layer, or another Group / Compound / ClipGroup). The new
  Group lands at the topmost target's original z-position
  (lowest index in `parent->children`); the targets become
  its children in parent z-order, preserving the layer's
  existing stacking regardless of the order the script
  listed them. Pushes `GroupNodesCommand` (undoable; redo
  re-wraps under a Group with the SAME iid so script-side
  state stays consistent). Returns the new Group's iid as
  String; binds to `result`. Returns empty string on: fewer
  than two args, any unresolvable iid, any out-of-scope-type
  iid, targets not sharing a parent, any target in a
  non-children slot. Atomic — no partial group ever lands.

- **`ungroup <iid>`** — dissolve the named Group and promote
  its children to the Group's parent at the Group's slot
  position. Pushes `UngroupNodeCommand` (undoable). Refuses
  (returns Null) on: empty iid, iid not resolvable, iid
  resolves to a non-Group type, the Group has no findable
  parent, the Group is owned by a non-children slot, the
  Group has no children. Mirrors `Canvas::ungroup_selection`'s
  refusal shape.

- **`find_by_name <name>`** — return the iid of the first
  in-scope scene object whose `name` matches exactly, or
  empty string on miss. Walks the doc tree depth-first;
  first hit wins. Names aren't unique by construction (the
  document name uniquifier only normalises load-time
  collisions; live collisions can happen). "First hit in
  walk order" matches `layers find_by_name` semantics. Read-
  shaped, but lives on `invoke` rather than `query` because
  the grammar's query path can't take arguments today.

- **`find_by_type <type>`** — return the iid of the first
  in-scope scene object whose type token matches, or empty
  string on miss. `<type>` vocabulary: the same ten lowercase
  tokens listed under **Type scope** above. Out-of-scope
  tokens (`layer`, `guide`, `guidelayer`, etc.) silently
  don't match — they return empty string rather than
  dispatching against the out-of-scope set. Useful for
  smokes that need to land an iid of a known type without
  pre-knowing one.

### Per-instance verbs (on `objects.<iid>`) — field edits

The five field-edit verbs all push `EditObjectFieldCommand`
on success. Direct mutation precedes the push (matches the
canvas-tool shape: the user perspective is "the verb did the
thing"; Ctrl+Z then undoes it).

- **`toggle_visible`** — flip `visible`. No arguments.
- **`set_visible <bool>`** — write `visible`. No-op (no
  command, no callback) if already in the requested state.
- **`toggle_locked`** — flip `locked`. No arguments.
- **`set_locked <bool>`** — write `locked`. No-op if already
  in the requested state.
- **`rename <name>`** — write `name`. Empty-string argument
  AND name-unchanged are both no-ops. The "" sentinel here
  means "no rename happened"; scripts that genuinely want to
  clear a name back to the freshly-minted empty state can't
  do so via `rename` today (same constraint
  `layers.<iid> rename` has, matching the panel's
  "empty-entry leaves the name alone" UX convention).

### Per-instance verbs (on `objects.<iid>`) — geometry

- **`set_path_data <d-string>`** — replace the Path's
  `PathData` from an SVG-d string. **Refuses on non-Path
  types** (Group / Compound / Image / Ref / etc. — silent
  no-op; the resolved node has no `path` slot to write
  into). Pushes `EditPathCommand` (undoable; the same
  s167-migrated whole-path replace command the canvas pen
  tool uses). The d-string is interpreted in **user space**
  — Y-up, origin at bottom-left of the artboard, the
  coordinate system the user sees on the canvas. The
  script-side seam flips user→doc before storing; the
  stored `PathData` and the renderer both work in doc space
  (Y-down). See **User-space coordinates** below for what
  this means in practice. No-ops: empty args, non-Path
  type, d-string parses to an empty path on a Path that's
  already empty.

### Per-instance verbs (on `objects.<iid>`) — appearance

Seven verbs over the SceneNode's `fill` / `stroke` axes. All
seven push `EditAppearanceCommand` (the s167-migrated whole-
FillStyle / whole-StrokeStyle replace plus binding-capture).
Direct mutation precedes the push. **Universal across
SceneNode subtypes** — every node carries `fill` /
`stroke`, so Group / Compound / etc. accept these verbs;
the visual effect depends on the container's render path.

**The colour token vocabulary** (used by `set_fill` and the
paint axis of `set_stroke`) is the same vocabulary the SVG
fill / stroke attributes accept, parsed by
`curvz::utils::fill_attr_to_fill_style`:

- **`none`** — no paint (the FillStyle::None variant).
- **`currentColor`** / **`currentcolor`** — defer to the
  document's `currentColor` (the FillStyle::CurrentColor
  variant). The case-insensitive spelling matches SVG.
- **`#rgb`** / **`#rrggbb`** / **`#rrggbbaa`** — hex colour
  literal. Short hex (`#abc`) expands the SVG way (`#aabbcc`).
- **`rgb(r,g,b)`** / **`rgba(r,g,b,a)`** — CSS function
  syntax. Channels in 0..255, alpha in 0..1.
- **Named CSS colours** (`red`, `cornflowerblue`, etc.) —
  the full CSS-named colour set.

The read-side `fill` / `stroke` queries always return the
**normalised** form (lowercase `#rrggbb` hex, `none`, or
`currentColor`); named colours and `rgb()` / `rgba()`
inputs normalise to hex on read.

**Direct-override convention.** A direct appearance write
breaks the affected paint's swatch binding and always breaks
the whole-node `bound_style` binding. Specifically:

| Verb | Clears `fill_swatch_id` | Clears `stroke_swatch_id` | Clears `bound_style` |
|---|---|---|---|
| `set_fill` | yes | no | yes |
| `set_stroke` | no | yes | yes |
| `set_stroke_width` | no | no | yes |
| `set_stroke_cap` | no | no | yes |
| `set_stroke_join` | no | no | yes |
| `set_stroke_miter` | no | no | yes |
| `set_stroke_opacity` | no | no | yes |

The reasoning: swatch bindings represent **paint** references,
so only paint-edits break the swatch; the whole-node
`bound_style` binding represents a style preset, and any
direct appearance edit drifts away from the preset so the
binding has to break to make the edit stick across a
subsequent style-side edit. All clears are undoable (the
command captures the binding before and after).

**No-op early-return.** Each verb checks the new value
against the current value first; if they're equal nothing
is pushed and the callback doesn't fire. For colour values
the comparison is `fill_style_equals_byte_rounded` — byte-
clean equality after the parse, so re-applying the same
colour twice in a row doesn't pile up undo entries.

Now the verbs themselves:

- **`set_fill <colour-token>`** — write `fill`. Colour
  vocabulary above. Refuses on empty args.
- **`set_stroke <colour-token>`** — write `stroke.paint`.
  Same vocabulary. Width / cap / join / miter / opacity are
  preserved. For a fresh-mint Path, `stroke.width` defaults
  to 0.0 — so `set_stroke "black"` alone produces no
  visible outline; pair with `set_stroke_width` to make it
  draw.
- **`set_stroke_width <number>`** — write `stroke.width`. No
  clamp / no refusal; the model accepts whatever the script
  supplies and Cairo handles edge cases. Non-numeric args
  fall back to the current width via `arg_as_double`'s
  fallback, which then hits the value-unchanged early-return
  — so an unparseable arg silently no-ops.
- **`set_stroke_cap <token>`** — write `stroke.cap`. Token
  vocabulary: **`butt`** / **`round`** / **`square`**
  (matches SVG's `stroke-linecap`). Unknown tokens silently
  no-op (no safe-default fallback the way colour has
  `currentColor`).
- **`set_stroke_join <token>`** — write `stroke.join`. Token
  vocabulary: **`miter`** / **`round`** / **`bevel`** (matches
  SVG's `stroke-linejoin`). Same unknown-token no-op policy.
- **`set_stroke_miter <number>`** — write `stroke.miter` (the
  miter limit — the ratio at which miter-join corners get
  clipped to a bevel; SVG default 4.0). Same numeric-arg
  shape as `set_stroke_width`.
- **`set_stroke_opacity <number>`** — write `stroke.opacity`,
  a 0.0–1.0 alpha multiplier applied to the resolved stroke
  paint at the Cairo seam. 1.0 = fully opaque; 0.0 =
  invisible. No clamp at the model layer (negative values
  zero out at Cairo's clamp; values >1.0 saturate at 1.0).
  This is **stroke-paint alpha** specifically; node-level
  opacity is a separate concept and isn't surfaced on
  `objects` today.

### Per-instance verbs (on `objects.<iid>`) — structural

The three structural verbs reorder or duplicate within the
tree. Each rides a different command class.

- **`move <direction>`** — reorder within the parent's
  `children` vector. Token vocabulary: **`up`** (idx-1,
  raise in z-order toward the top), **`down`** (idx+1,
  lower in z-order toward the bottom), **`top`** (idx=0),
  **`bottom`** (idx=last). Mirrors `layers.<iid> move`'s
  vocabulary. Z-order convention: lower index = visually on
  top (children[0] paints first, children[last] paints
  last, later paints over earlier). Pushes
  `MoveNodeIndexCommand` (single-target, iid-keyed). No-ops:
  unknown direction, parent has only one child, target
  already at the destination edge (`up`/`top` on idx 0;
  `down`/`bottom` on idx last), target owned by a non-
  children slot.

- **`reparent <new_parent_iid>`** — move the node into the
  named container's `children` at position 0 (front-insert,
  the "new on top" convention). Pushes
  `MoveObjectToLayerCommand` (the name is layer-historical;
  the command is mechanically container-agnostic and walks
  the whole project tree). Refusals (all no-ops): empty
  arg, destination iid not resolvable, destination is a
  leaf type (Path / Text / Image / Ref / Measurement — can't
  hold children), destination equals self, destination is a
  descendant of self (cycle), source is owned by a non-
  children slot. Cross-container moves into Group /
  Compound / ClipGroup are accepted; cross-doc reparents
  work mechanically (the command is iid-keyed) but aren't a
  normal use case.

- **`duplicate`** — clone the node and its full subtree;
  insert the clone at the original's slot index (the
  duplicate ends up on top of the original; matches canvas
  alt-drag-duplicate convention). The clone has **fresh
  internal_ids on every node in its subtree** so the iid
  index can hold both distinctly. SVG `id` and `name` are
  NOT freshened — script-minted objects start with empty
  `id` (assigned lazily at SVG-export time) and the script
  controls naming via `rename`. Pushes `DuplicateCommand`.
  Returns the new top-level iid as String; binds to
  `result`. Returns empty string when source is owned by a
  non-children slot (same refusal shape as `delete` /
  `move` / `reparent`).

### Collection-scope properties (on `objects`)

- **`count`** — Int. Number of in-scope scene objects in the
  **active doc**, summed recursively across all layers
  (every layer's children, deep-walked). Returns 0 if no
  project loaded or no active doc.
- **`all_iids`** — String. Comma-separated iids of every
  in-scope scene object in the **active doc**, in walk
  order (depth-first within each top-level layer; layers in
  their natural vector order). No trailing comma. Empty
  string if `count == 0`.

### Per-instance properties (on `objects.<iid>`)

Seventeen properties. Eight describe identity and tree
position; two describe geometry; seven describe appearance.

**Identity and tree position:**

- **`name`** — String. The node's display name. Empty for
  freshly-minted objects until `rename` runs.
- **`type`** — String. Lowercase type token, same vocabulary
  as `find_by_type` (`path`, `group`, `compound`,
  `clipgroup`, `blend`, `warp`, `text`, `image`, `ref`,
  `measurement`).
- **`visible`** — Bool.
- **`locked`** — Bool.
- **`iid`** — String. `node.internal_id`. Useful in tests: a
  script can `get objects.<iid> iid` to verify it has the
  right handle.
- **`parent_iid`** — String. The iid of the node's parent
  container. For a Path that lives directly under a Layer,
  `parent_iid` is the Layer's iid. For a Path inside a
  Group inside a Layer, `parent_iid` is the Group's iid.
  Empty string if the parent walk doesn't find a match
  (shouldn't happen for a resolved iid — defensive).
- **`child_count`** — Int. Number of direct children (size of
  `node->children`). Always defined; 0 for leaf types
  (Path / Text / Image / Ref / Measurement) in practice.
- **`child_index`** — Int. The node's position within its
  parent's `children` vector. **-1** if the node is owned by
  a non-children structural slot (`clip_shape`,
  `blend_source_a`, `blend_source_b`, `warp_source`) — those
  are structural inputs to their containers, positioned by
  identity not by list index. Also -1 if the parent walk
  can't find a slot at all (defensive). Used by scripts to
  observe `move` / `reparent` / `duplicate` round-trips.

**Geometry:**

- **`node_count`** — Int. Size of the Path's `BezierNode`
  vector. Returns **0 for non-Path types** by contract (the
  `node->path` slot is nullptr; "no path" reads as "no
  nodes"). The observable for `set_path_data` — pure
  model-side, no Cairo, deterministic across runs.
- **`path_data`** — String. The Path's `PathData` re-emitted
  as an SVG-d string in **user space** (the same coordinate
  system `set_path_data` accepts). Returns empty string if
  `path` is null or empty. **Not byte-identical to the most-
  recent `set_path_data` input** — fmt2 normalises decimals
  and segment classification may shift between `L` and `C`
  based on handle degeneracy. The read surface for capture /
  inspection use cases; for byte-clean round-trip a future
  structural probe would parse both and compare PathData.

**Appearance:**

- **`fill`** — String. The node's `fill` re-emitted as an SVG
  fill-attribute string via
  `curvz::utils::fill_style_to_fill_attr`. Normalises to
  lowercase `#rrggbb` for solid colours; returns `none`,
  `currentColor`, or `#rrggbb` for the context-free variants.
  Swatch-bound or gradient fills emit a `#rrggbb` fallback
  (the first stop or remembered solid colour) — round-trip
  through script is lossy on those cases by design (the
  swatches / styles surface is the right path for those).
  **Default for a fresh-mint Path**: `currentColor`.
- **`stroke`** — String. The node's `stroke.paint` (the colour
  axis only) re-emitted via the same pump. Same normalising
  behaviour. **Default for a fresh-mint Path**: `currentColor`.
- **`stroke_width`** — Double. `node.stroke.width`. **Default
  for a fresh-mint Path**: 0.0 — which is why a fresh-mint
  Path renders no outline even when `stroke` is a visible
  colour.
- **`cap`** — String. Token form of `node.stroke.cap`:
  `butt` / `round` / `square`. **Default**: `butt`.
- **`join`** — String. Token form of `node.stroke.join`:
  `miter` / `round` / `bevel`. **Default**: `miter`.
- **`miter`** — Double. `node.stroke.miter` (the miter limit).
  **Default**: 4.0 (matches SVG).
- **`stroke_opacity`** — Double. `node.stroke.opacity` (the
  stroke-paint alpha multiplier). **Default**: 1.0 (fully
  opaque).

**ScriptValue strict-equality kink** — worth flagging because
it bites smokes. `assert objects.<iid> stroke_width == 2.0`
matches a Double query against a Double-kind RHS and passes.
`assert objects.<iid> stroke_width == 2` matches Double
against Int-kind and **fails** (different kinds; no implicit
coercion). The Double-typed properties on this page are
`stroke_width`, `miter`, `stroke_opacity` — write the RHS
literal with a decimal point.

### Naming notes

- **`stroke_opacity` vs `opacity`.** The property is spelt
  `stroke_opacity` (not `opacity`) because a node-level
  opacity multiplier is a separate concept and `opacity`
  alone would be ambiguous. `layers.<iid> opacity` exists
  for the per-layer multiplier; the per-node multiplier
  isn't surfaced on `objects` today.
- **`miter` (property) vs `set_stroke_miter` (verb).** The
  query drops the `stroke_` prefix because the proxy IS the
  object — the prefix repeats context (same convention as
  `cap` and `join`). The verb keeps the prefix because the
  verb name has to be unambiguous across all Scriptables
  (`StylesScriptable` surfaces a `stroke_miter_limit`; the
  prefix disambiguates).
- **`find_by_iid` isn't a verb.** A script that has an iid
  in hand goes straight to `objects.<iid>` addressing — no
  query needed. `find_by_name` and `find_by_type` exist
  because scripts often start from a property and need to
  bind an iid; `find_by_iid` would be an identity function.

### User-space coordinates

`set_path_data` and the `path_data` read query both speak
**user space** — the coordinate system the user sees on the
canvas. Y-up, origin at the bottom-left of the artboard. The
engine stores `PathData` in doc space (Y-down, top-left
origin) because that's what Cairo and the SVG IO path use;
the user-space ↔ doc-space flip happens at the script-side
seam only.

Practically, this means a Path created with
`set_path_data "M 0 0 L 100 100 Z"` lands at the user-space
origin (bottom-left of the artboard, not top-left), with the
`L 100 100` heading toward the top-right by the user's eye.
The read query echoes the same orientation back: the
re-emitted d-string starts from the user-space coordinates,
not the stored doc-space ones.

The flip uses the existing `CoordSpace` Y-flip seam through
two `curvz::utils` pumps (`path_data_user_to_doc` and
`path_data_doc_to_user`). `EditPathCommand` undo / redo are
**unaware** of user-space — they round-trip the stored doc-
space `PathData` exactly. Only the script-side seam crosses.

### Examples

Create a triangular Path with a red fill and a 2px black
stroke, then verify each property landed:

```
layers new
set lid to result
layers.lid rename "objects smoke"
layers.lid activate

objects new "path"
set p to result
objects.p rename "triangle"
objects.p set_path_data "M 0 0 L 100 0 L 50 100 Z"
objects.p set_fill "#ff0000"
objects.p set_stroke "#000000"
objects.p set_stroke_width 2.0
objects.p set_stroke_cap "round"
objects.p set_stroke_join "round"

assert objects.p type == "path"
assert objects.p node_count == 3
assert objects.p fill == "#ff0000"
assert objects.p stroke == "#000000"
assert objects.p stroke_width == 2.0
assert objects.p cap == "round"
assert objects.p join == "round"
```

Round-trip an iid through `all_iids` and reorder one
child:

```
get objects all_iids
set ids to result
# Output: ids = "<iid-1>,<iid-2>,<iid-3>"
# Driver splits on comma, picks one, then:
objects.<iid-2> move "top"
get objects.<iid-2> child_index
# Output: objects.<iid-2> child_index = 0
```

Group two siblings and verify the result:

```
objects new "path"
set a to result
objects new "path"
set b to result
objects group a b
set g to result
assert objects.g type == "group"
assert objects.g child_count == 2
objects ungroup g
```

Tear down without leaving residue (the smoke pattern from
the `34_objects_scriptable_smoke` test):

```
get objects count
set count_before to result
# ... build up, validate ...
objects delete a
objects delete b
layers delete lid
get objects count
assert objects count == count_before
```

## ❷ Canvas widget — substrate widgets only

The canvas widget itself is **not** a Scriptable. There's no
`canvas` registry entry; `list` from the Scripter doesn't
show it. The canvas is the rendering surface the toolbar
tools draw onto and the inspector reads from — script
addressability for canvas operations goes through the
indirect surfaces: `objects` for scene content, `inspector`
for section state, `win` for menu-driven canvas commands
(zoom in, zoom out, fit to window, view toggles).

The canvas widget does carry two `set_name` substrate ids
that show up in the Scripter's widget-name resolver — the
same way Toolbar buttons and Inspector spinners do. They're
not script-callable verbs; they're addressable names that
`win` actions or future canvas-side Scriptables could
reference.

### Substrate widgets on the canvas

- **`cv` / `canvas_drawing_area`** — the main drawing area
  (`Gtk::DrawingArea`). The widget that catches mouse and
  keyboard events for the canvas; the rendering target every
  tool draws into. Set in `Canvas.cpp` during canvas
  construction.
- **`cnv_txt_ent` / `canvas_text_tool_overlay_entry`** — the
  transient overlay `Gtk::Entry` the Text tool spawns when
  the user clicks to start typing. Lives only while the Text
  tool's edit session is open; created and torn down per
  edit. Set in `Canvas_ops.cpp` as part of the text-tool
  overlay path.

### Canvas operations through other Scriptables

For canvas operations a script wants to drive today, the
surface lives elsewhere:

- **Zoom / pan / fit** — `win` actions (`win.view-zoom-in`,
  `win.view-zoom-out`, `win.view-zoom-fit-window`, etc. —
  see **Header & menus reference** for the full action list).
- **View toggles** (rulers, grid, guides, margins) — `win`
  actions on the View menu.
- **Active tool selection** — Toolbar substrate widgets
  (`tb_<tool>`) clicked via the widget-name resolver — see
  **Toolbar reference**.
- **Inspector section state** (canvas size, theme, motif) —
  `inspector` singleton verbs — see **Singletons reference**
  and the Inspector area of **Inspector reference**.
- **Scene content** — `objects` collection above.

A future canvas-side Scriptable (working name `canvas`)
could surface zoom level, pan offset, and view-toggle state
as direct properties — but today there's nothing of the
sort. The closest read-side observable is `inspector get
<section> ...` for inspector-mediated state.

## Reference complete for `objects`

This page closes the `objects` arc on the documentation side
— every shipped verb and property on the collection and the
proxy is enumerated above. The remaining Scripter reference
page — **Dialogs & popovers** — covers the modal dialog
widget catalogue (Blend, Gradient, Style editor, Theme
editor, Export, Print, Step and Repeat, New document, Save
As, and the toolbar popovers). With Dialogs & popovers the
eight-leaf reference set is complete.
