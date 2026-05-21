# Layers

The **Layers** panel shows the structure of the document you are
currently editing. Every path, group, guide, reference image, and
measurement annotation appears as a row, indented to show how things
nest. The active layer is the one new objects land in; the active
selection is highlighted in a brighter tint so the panel always
reflects what's selected on the canvas, and vice versa.

Curvz documents always have at least one layer. You can add more to
keep complex icons organised — base shapes on one layer, fine
details on another, construction guides on a third.

![The Layers panel with two layers expanded](img/6-2-layers-stack.png)

## ❶ Add and delete layers

The header row ❶ at the top of the panel has two buttons: **+** to
add a new empty layer, **−** to delete the currently selected one.
The new layer goes above the active layer in the stack and becomes
active itself.

Curvz refuses to delete the last remaining content layer — every
document needs somewhere for objects to live. Special layers (guide,
grid, margin, reference, measure) can be deleted; deleting a guide
layer also clears all the guides on it.

## ❷ The macro strip

Above the layer list is a collapsible **Macros** ❷ section. It lists
every macro you've starred in the Macro Manager so you can run them
in one click without leaving the canvas. The strip starts collapsed
the first time it appears and remembers your last toggle state for
the rest of the session.

The strip is hidden entirely when no macros are starred — it
appears as soon as you star one. Star a macro in **Macro Manager**
(Ctrl+Shift+M) to populate it. See **Macros** (11.1) for the full
macro story.

## ❸ Layer rows

Each layer row ❸ has, from left to right:

- A **collapse arrow** (▾ / ▸) to fold the layer's contents.
- A small **colour dot** drawn from the layer's palette colour. Click
  to open a colour picker. Layers cycle through seven palette
  colours by default; the dot is purely a visual tag for the panel
  — it doesn't affect rendering.
- The **layer name** (double-click to rename).
- **Visibility** (eye) and **lock** (padlock) toggles on the right.

A locked layer can be shown but not edited: the canvas refuses
clicks, drags, and Delete on objects inside it. Lock toggles take
effect at the next interaction without rebuilding the panel.

## ❹ Object rows

Inside an expanded layer ❹ are rows for the layer's objects: paths,
groups, compound paths, blends, text, images, and so on. Each row
shows a small icon for its type and the object's name. Selection is
synchronised: clicking a row selects the object on canvas, and
canvas selection highlights the corresponding row.

**Multi-select** works on both axes:

- Ctrl-click adds and removes individual rows.
- Shift-click extends a contiguous range.
- Marquee-selecting on the canvas updates the panel highlights to
  match.

## ❺ Drag-and-drop reordering

Rows can be dragged. As you drag, Curvz draws a coloured drop
indicator showing where the row will land:

- **A bar above or below** a row means "place at this z-position
  outside any group."
- **A frame around** a group or layer row means "drop *inside* this
  container."

Layers can be reordered with respect to each other; objects can move
between layers by dragging into a different layer's frame. Group
rows accept drops the same way — drop an object onto a group's frame
and it joins the group.

Layer order in this panel matches the SVG render order: rows nearer
the top draw on top of rows below them. Within a layer, objects
follow the same convention. Drag-reordering rearranges rendering as
well as the panel.

> **Note:** drag-and-drop on compounds, deep groups, and reference
> images is occasionally inconsistent — if a drop doesn't take, try
> starting the drag from a different part of the row, or use the
> menu-bar verbs (Path → Group / Compound / Clip) on the canvas
> selection to compose the relationship explicitly.

## ❻ Special layer types

A few row types ❻ exist for non-content layers:

- **Guide layers** hold the document's guide lines. The row icon is
  a small alignment guide; opening the layer reveals one row per
  guide.
- **Grid** and **Margin** layers are the document's grid and margin
  rectangles. They can be hidden and locked but not deleted via the
  − button while the inspector toggles control them.
- **Reference image layers** hold imported raster references — your
  template, your sketch, the icon you're matching. They render
  beneath everything but don't export.
- **Measurements layer** — holds the persistent measurements
  drawn with the **Measure** tool (4.6.3). Each measurement is
  one row.
- **Blend rows** appear when a blend operation has produced an
  intermediate-step output. The row's right-click menu includes
  **Rebuild Blend Steps** for forcing a cache refresh after
  editing the source paths (see Right-click context below).

## Right-click context

Right-click an **object row** (path, group, compound, clip group,
blend, warp, text, image) to open its context menu. The same menu
appears whether you right-click a top-level row or a nested child,
adapting to the row's depth and type. Right-clicking a row that
isn't currently part of the canvas selection first promotes that
row to be the selection, then opens the menu — the verbs always
act on what the menu says they will. Right-clicking a row already
inside a multi-selection keeps the multi-selection and the verbs
act on every member.

The menu contents:

- **Delete** — always present. Removes every selected object,
  same as the Delete key on canvas.
- **Move to layer ▸** — submenu listing every ordinary unlocked
  layer in the document, minus the home layer if every selected
  object already lives in the same layer (moving to where you
  already are is a no-op). Only appears when every selected
  object is **top-level** in its layer. If any selected object
  is nested inside a container (group, compound, clip group,
  blend, or warp), this submenu is suppressed — release the
  nested object from its container first.
- **Release verb** — surfaced when the selection is a single
  container row, naming the verb appropriate to the type:
  *Ungroup* for Group (`Ctrl+Shift+G`), *Release Compound* for
  Compound (`Ctrl+Shift+8`), *Release Clip* for Clip Group
  (`Ctrl+Alt+7`), *Release Blend* for Blend (`Ctrl+Shift+B`),
  *Release Warp* for Warp (no hotkey).
- **Rebuild Blend Steps** — only on a single Blend selection,
  below Release Blend. Forces the cached intermediate steps to
  refresh on the next draw, for the rare case where editing a
  source didn't trigger an automatic rebuild.

Three row types are intentionally menu-less by construction:
**Reference points** (these belong to the Refpoints layer and
can't move between layers); **Clip-shape pseudo-rows** inside a
Clip Group (the clip shape is structurally tied to its group —
right-click the group instead); and **Blend A / B / Steps
pseudo-rows** (these are inline references into the parent Blend's
slots — right-click the Blend row itself).

> **Layer header rows have no right-click menu yet.** Layer-level
> verbs (Duplicate Layer, Merge Down, Move Up / Down, Lock /
> Visibility toggles) are reachable through other surfaces — the
> ▲▼ arrow buttons on the row, the eye and padlock toggles, the
> ✎ rename gesture. Right-click on layer headers is a logged
> follow-up but isn't wired today.

The same Move to layer ▸ submenu and the same release verbs are
also available from the **canvas right-click context menu** (4.2.1)
as of s275 m12, alongside the canvas's lifecycle / arrange /
translate / effects verbs. Use whichever surface is closer to
your cursor; both push the same commands onto the same undo
history.

## Where to next

- The arrange menu and z-order keys are documented in
  **Editing paths** (8.1).
- For panels that hold *reusable* assets across documents — clip
  art, named colours, named styles — see **Library**
  (6.3), **Swatches** (6.4), and **Styles** (6.5).
- The arrow shortcuts below send the canvas selection through
  z-order within its parent. They are the keyboard counterpart
  to the **Arrange** submenu in the canvas right-click menu
  (4.2.1) and the **Path → Arrange** menu-bar verbs.

### Keys

These bindings act on the canvas selection — the layer panel
mirrors what the canvas is doing.

- `Ctrl + ↑` — bring the selection forward by one z-step.
- `Ctrl + ↓` — send the selection backward by one z-step.
- `Ctrl + Shift + ↑` — bring to front (top of layer).
- `Ctrl + Shift + ↓` — send to back (bottom of layer).
- `Ctrl + G` — group the selection.
- `Ctrl + Shift + G` — ungroup.
- `Ctrl + Tab` / `Ctrl + Shift + Tab` — cycle to the next /
  previous **document** (not the next layer). Layer cycling is
  done by clicking layer rows.
- `Delete` / `Backspace` — delete the selection (respects layer
  locks).

The Macro Manager opens with `Ctrl + Shift + M`, which is the only
hotkey that touches the macro strip directly. See **Macros** (11.1).
