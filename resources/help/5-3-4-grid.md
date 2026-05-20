# Grid

The **Grid** section configures the document's grid overlay — a
regularly-spaced lattice drawn behind your artwork. Like guides
and margins, the grid is document-scoped, never exports with the
SVG, and exists only to help you place things precisely while
drawing.

The grid is **off by default**. Enable it from this section, then
configure spacing, offset, style, and colour.

> **Section location:** Grid sits inside the **Theme** disclosure
> (5.3.8) alongside Canvas (surface colours) and Margins. The
> three travel together in the saveable theme bundle — applying a
> theme reapplies the grid's geometry and colour to match.

![The Grid section with the grid enabled and Spacing/Offset rows visible](img/5-3-4-grid-section.png)

## ❶ Enable

A single checkbox at the top of the section toggles the grid on
and off. With the grid disabled, the rest of the section is
hidden and the canvas shows no overlay. Tick the checkbox and
the section expands to its full set of controls; the canvas
immediately starts drawing the grid using the most recent
configuration (or sensible defaults if you have never enabled
the grid in this document before).

Disabling the grid does not lose your settings — Curvz keeps
the grid layer's geometry around between toggles within a
session. Re-enabling restores everything you had.

## ❷ Style

A two-option dropdown — **Lines** or **Dots**:

- **Lines** draws the grid as horizontal and vertical thin
  lines crossing the entire canvas. Best for general layout
  work where you want a continuous reference frame.
- **Dots** draws only the grid's intersection points as small
  marks. Best for icon work, where lines can be visually noisy
  against detailed artwork.

The choice is purely cosmetic — both modes use the same spacing,
offset, and colour.

## ❸ Spacing

A pair of numeric fields under a **SPACING** label:

- **X** — horizontal distance between grid columns.
- **Y** — vertical distance between grid rows.

Both spinners respect the document's display unit (set in the
**Dimensions** section, 5.3.2). Edits commit on Enter or focus
loss and update the canvas live.

You almost always want **X = Y** for an isometric-feeling grid.
Setting them differently is useful when the artwork itself has
an asymmetric repeat — typesetting layouts, for example, often
use a coarser horizontal grid than vertical.

## ❹ Offset

A second pair of numeric fields under an **OFFSET** label:

- **X** — horizontal shift of the entire grid.
- **Y** — vertical shift of the entire grid.

Without offset, the grid's first column or row sits at zero in
document coordinates. Offsets push the whole lattice so the
first line lands somewhere else. This is useful when you want
to align the grid with the artwork rather than the artboard
origin — for example, offsetting a 16-px grid by 8 px in both
axes centres a cell on the origin instead of starting one there.

## ❺ Color

A wide colour swatch shows the grid's current colour. Click to
open the colour picker, which here also exposes alpha — the
grid is drawn translucently by default so it does not compete
with the artwork. A typical choice is a desaturated tint at
20–40% alpha, dark on a light artboard and light on a dark one.

The grid colour is a single document-wide value. There is no
per-axis or per-step colouring (and adding one is a backlog
item).

## Snap to grid

The grid is just an overlay — the cursor does not snap to it
unless you explicitly opt in. Snap toggles are no longer an
inspector section; they live in the **Snap toolbar popover**
(right-click the Snap button, **Q**). The **Snap to grid**
toggle there catches dragging or nudging on the grid's
spacing+offset lattice.

## Locking and disabling

The grid lives on its own document layer (`Grid`) which can be
locked from the **Layers** panel. When the grid layer is locked,
all controls in this section grey out so you cannot edit grid
geometry without first unlocking it on the layer side.

Unticking **Enable** at the top of the section *removes* the
grid layer entirely from the document. If you only want to
hide the grid temporarily — say, for a screenshot — toggle the
grid layer's visibility from the Layers panel instead. That
keeps your geometry intact.

## Where to next

The grid pairs with **Snap to grid** in the Snap toolbar popover
(see above). The other layout overlays are **Margins** (5.3.5),
which sits next to Grid inside the Theme disclosure, and
**Guides**, which moved to the Object group in s179. All three
follow the same shape: an enable toggle, geometry fields, and a
colour swatch.
