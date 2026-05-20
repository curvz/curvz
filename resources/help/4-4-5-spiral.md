# ![Spiral tool icon](img/icons/curvz-spiral-symbolic.svg) Spiral

The **Spiral tool** drops a parametric spiral — a path that winds
inward from an outer radius to an inner radius over a chosen
number of turns. Like Polygon, it has both a quick drag-to-draw
flow and a right-click popover for full configuration with an
interactive preview.

Activate it from the toolbox or with the **W** key (canvas focus
required).

![The Spiral popover with the interactive preview](img/4-4-5-spiral-context.png)

## Drag to draw

The drag flow is identical in shape to Polygon's:

1. Press the mouse on the canvas at the **centre** of the spiral.
2. Drag outward — the drag distance becomes the spiral's
   **outer radius**, and the drag direction sets the **rotation
   angle** of the outer tip.
3. Release to commit.

Number of turns and inner radius come from the popover's last
values — by default a few turns, with the inner radius pulled in
to a small percentage of the outer.

After commit, the new spiral becomes the selection and Curvz
returns to the **Selection tool**.

## Modifiers while dragging

- **Shift** — constrains the rotation to a **15° increment**, the
  same way it does for Polygon. Useful for spirals whose tip
  needs to land on a clean angle.

There is no Alt-modifier drag-from-edge variant for Spiral; the
press point is always the centre.

## Right-click for full configuration

Right-click the Spiral button in the toolbox to open the **Place
Spiral** popover:

### ❶ Interactive preview

A 160×160 dark-themed preview at the top renders the spiral with
the current settings — exactly the path the **Place** button
would commit. The preview uses the same `spiral_to_path`
generator the canvas does, so what you see is what you get.

A small dot marks the spiral's centre. The path itself draws in
the standard preview blue with rounded line caps for clarity.

### ❷ Turns

A spinner from 0.25 to 20.0 controlling how many full revolutions
the spiral makes from outer to inner. Fractional values are
allowed — 1.5 turns gives a half-revolution past one full loop.

For tight spirals (a clock-face spiral, a snail-shell spiral),
2 to 4 turns is typical. For loose decorative spirals, fewer
turns work better. Very high counts produce a dense
visually-saturated coil that reads as a filled disc at icon
sizes.

### ❸ Inner

A spinner from 0 to 95%, labelled **Inner r %:**, controlling the
**inner radius as a percentage of the outer**. Small values
(5–20%) produce spirals that nearly close on themselves at the
centre. Large values (50%+) produce open spirals that look more
like ring fragments than full coils.

The inner-percent and turns values together define the spiral's
density — fewer turns plus a larger inner radius gives an open,
airy spiral; more turns plus a smaller inner radius gives a
tightly-wound coil.

### ❹ CX / CY / R

Numeric placement fields:

- **CX**, **CY** — the spiral's centre position in document
  coordinates.
- **R** — the outer radius.
- **Units** — read-only label showing the active display unit.

### ❺ Place

Commits the spiral and dismisses the popover. The drag flow
remains available for follow-up shapes; the popover values seed
the next drag's defaults.

## What the spiral produces

The committed path is an **open** path — a spiral has explicit
endpoints (the outer tip and the inner tip), not a closing
segment. The path uses cubic Bézier segments tuned to approximate
the spiral curve, with the node count scaling with the number of
turns.

Inherits fill and stroke from the toolbar's fill/stroke well.
Most spirals look right with stroke-only and fill set to None —
filling a spiral creates a swirly disc shape with self-intersecting
fill rules.

## When to use a Spiral

The Spiral tool is a parametric — exactly right when you need a
mathematically-clean coil and you don't want to draw it node by
node. Common icon use cases:

- Loading and refresh indicators (with a partial turn count).
- Clock or watch face decorations.
- Decorative flourishes and ornament details.
- Galaxy / spiral-shape iconography.

For irregular spirals (one that fades to a different shape, or
one whose curvature changes mid-path), draw a base spiral here
and then refine it with the **Node tool** (4.2.2).

## Where to next

The natural neighbours are **Polygon** (4.4.4) — the sibling
parametric tool with its own interactive popover — and the
**Pen** (4.3) for spirals you want to draw freehand. The **Node
tool** (4.2.2) is what you reach for to refine any committed
spiral.

For decorative spirals that need a tapering stroke, draw the
spiral here, then use the **Blend** feature (7.3) — blend the
spiral against a thinner-stroked copy of itself to get a width
falloff effect. Curvz does not have variable-width strokes
directly today.
