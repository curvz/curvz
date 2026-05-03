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
on a fresh project — its arrow expands it. The expanded state is
remembered across sessions on a per-document basis.

If you haven't starred any macros, the section stays empty. Star a
macro in **Macro Manager** (Ctrl+Shift+M) to populate it. See
**Macros** (11.1) for the full macro story.

## ❸ Layer rows

Each layer row ❸ has, from left to right:

- A **collapse arrow** (▾ / ▸) to fold the layer's contents.
- A small **colour dot** drawn from the layer's palette colour. Click
  to open a colour picker. Layers cycle through eight palette
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

> **Note:** drag-and-drop on compounds, deep groups, and reference
> images is occasionally inconsistent — if a drop doesn't take,
> right-click the row and use the **Move to back / front** entries
> as a workaround.

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
- **Measure layers** hold the persistent measurements drawn with the
  Ruler tool. Each measurement is one row.
- **Blend rows** appear when a blend operation has produced an
  intermediate-step output. Right-click the blend row to rebuild
  the steps after editing the source paths.

## Right-click context

Right-click any row to get its context menu. The exact items vary by
row type:

- Layers: rename, delete, move up / down, change palette colour.
- Paths: rename, raise / lower in z-order, move to back / front,
  send to a different layer, group / ungroup.
- Blends: rebuild steps, release.
- Compound paths: split.

Layer order in this panel matches the SVG render order: rows nearer
the top draw on top of rows below them. Within a layer, objects
follow the same convention.

## Where to next

- The arrange menu and z-order keys are documented in
  **Editing paths** (8.1).
- For panels that hold *reusable* assets across documents — clip
  art, named colours, named appearance bundles — see **Library**
  (6.3), **Swatches** (6.4), and **Styles** (6.5).
- The right-click "Send to back / front / forward / backward" verbs
  also surface as the four arrow shortcuts below.

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
