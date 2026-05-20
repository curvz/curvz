# ![Polygon tool icon](img/icons/curvz-polygon-symbolic.svg) Polygon

The **Polygon tool** drops regular polygons and stars. The same
tool covers both — a star is just a polygon with the inner-vertex
radius pulled inward. Drag on the canvas to set radius and
rotation; right-click the toolbox button for a full configurator
with an interactive preview.

Activate it from the toolbox or with the **G** key (canvas focus
required).

![The Polygon popover with the interactive preview and snap labels](img/4-4-4-polygon-context.png)

## Drag to draw

The drag flow:

1. Press the mouse on the canvas at the **centre** of the polygon.
2. Drag outward — the drag distance becomes the polygon's
   **radius** (centre-to-vertex), and the drag direction sets the
   **rotation angle** of the first vertex.
3. Release to commit.

After commit, the polygon becomes the selection and Curvz returns
to the **Selection tool**.

The default shape is a regular polygon with the **side count and
star ratio set in the popover** — see below for both. With Curvz's
defaults, drag-to-draw produces a regular polygon (no star
inflection).

## Modifiers while dragging

- **Shift** — constrains the rotation to a **15° increment**.
  Useful for polygons aligned to standard angles — point-up
  triangles, axis-aligned squares-as-quad-polygons, and so on.
- **Alt** — currently a no-op (the press point is always the
  polygon's centre, regardless of Alt). The Alt-modifier slot is
  reserved for a future "drag from edge" mode.

## Right-click for full configuration

Right-click the Polygon button in the toolbox to open the **Place
Polygon / Star** popover. This popover is more elaborate than the
Rectangle / Ellipse / Line ones because polygon-specific
parameters need a visual feedback loop:

### ❶ Interactive preview

A 160×160 dark-themed preview at the top shows the polygon you'll
get with the current settings. Drag the small **handle** on the
preview's first bisector to adjust the **inflection** — pulling
inward makes it a star, pushing outward to the perimeter makes it
a regular polygon.

The preview highlights two snap states with a green stroke and a
caption:

- **"polygon"** — inflection is at the top of the range; the
  shape is a regular polygon.
- **"perfect star"** — inflection matches the geometric ratio
  `cos(2π/sides) / cos(π/sides)`, producing the canonical
  pointed-star shape for that side count. Available for 5+ sides.

Both snap zones are sticky — the handle latches when you drag
near them — so you don't have to be pixel-perfect.

### ❷ Sides

A spinner from 3 to 64. Sets the number of **vertices** on the
polygon — for a regular polygon, this is the edge count; for a
star, the point count.

### ❸ Inflection

A spinner from 1 to 100%, labelled **Inflect %:** — the same
value the preview handle sets visually. 100% is a regular
polygon; lower values pull the alternating vertices toward the
centre, producing a star. Near the bottom of the range the inner
vertices approach the centre and the shape reads as thin radial
spikes.

The spinner and the preview drag stay in sync — edit either, the
other follows.

### ❹ CX / CY / R

Numeric placement fields:

- **CX**, **CY** — the polygon's centre position in document
  coordinates.
- **R** — the centre-to-outer-vertex radius.
- **Units** — read-only label showing the active display unit.

### ❺ Place

Commits the polygon and dismisses the popover. The drag-to-draw
flow on the canvas remains available for follow-up shapes — the
popover values seed the *next* drag's defaults.

## Stars: a worked example

To draw a five-pointed star:

1. Right-click the Polygon button.
2. Set **Sides** to 5.
3. Drag the inflection handle until the preview labels itself
   "perfect star". (Or set inflection to about 38%, which is the
   geometric value for 5 sides.)
4. Set CX, CY, and R to whatever you want, or click **Place** and
   move it after.

For other star counts, the same workflow holds — the "perfect
star" snap takes the guesswork out for sides 5 through any
practical count.

## What the polygon produces

The committed path is a closed polygon with **N** nodes for a
regular polygon, or **2N** nodes for a star (alternating outer
and inner). Every node is a **Corner** type with retracted
handles — the polygon edges are straight lines.

Inherits fill and stroke from the toolbar's fill/stroke well.

To **round the corners** of a polygon, use the **Corner** tool
(4.8) on the selected nodes. Curvz does not bake corner radius
into the Polygon tool itself — the same rounding workflow applies
to every angular shape.

## Where to next

For organic curves, the **Pen** (4.3) and **Spiral** (4.4.5)
tools cover what Polygon doesn't. For circles or ellipses (which
are technically polygons in the limit), the **Ellipse** tool
(4.4.2) is more direct.

The **Corner** tool (4.8) pairs well with the Polygon tool for
icon work — draw a star, round its corners — that's a common icon
shape in two clicks.
