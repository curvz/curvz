# ![Measure tool icon](img/icons/curvz-ruler-symbolic.svg) Measure

The **Measure tool** measures distance and angle between two points
on the canvas. It picks **node and reference-point endpoints** as
its anchor positions — measurements snap to existing artwork, so
you get exact distances rather than eyeballed ones.

Activate it from the toolbox or with the **M** key (canvas focus
required). For its behaviour preferences (save measurements,
delete on copy), **right-click the Measure tool button** in the
toolbox — the popover holds both toggles. Those preferences
govern what happens *after* you complete a measurement; this
page covers the picking flow.

![A completed measurement showing two endpoint markers and the labelled triangle between them](img/4-6-3-measurement.png)

## Picking endpoints

A measurement is two endpoints: **A** and **B**. Plain clicks
walk you through the pair:

- **1st click** on a node or refpt — sets that as **A**. A
  dashed track line follows the cursor so you can see the tool
  is in flight.
- **2nd click** on a different node or refpt — sets that as **B**
  and completes the measurement. If **Save measurements** is on
  in the popover, the completion auto-saves to the Measurements
  layer.
- **3rd click** — starts a fresh measurement. The previous
  measurement is replaced (or, if saved, left in place as a
  saved annotation while you build the next one).

Re-clicking A while only A is set is a no-op — use Space (below)
to clear if you want to start over.

The 8-pixel pick tolerance is in screen space, so it scales with
zoom. Zoom in for fine work; zoom out for forgiving snaps over
larger distances.

## Shift+click — corrections

**Shift + click** is the corrective gesture for when you've
mispicked:

- **Shift+click an already-set A** — drops A, promotes B (if
  set) to A, clears B. Equivalent to "undo my first pick."
- **Shift+click an already-set B** — drops B only; A is kept.
- **Shift+click a fresh node** — promotes the current A to B
  and sets the new node as A. Completes the pair if A was set
  alone, with auto-save firing if the flag is on.

The third case is useful when you've picked A, then realise you
want to keep that point as the *second* endpoint with a new A
elsewhere — one shift-click does the swap.

## Marquee shortcut

Drag a **marquee** on empty canvas to box-select endpoints
inside the rectangle:

- **Exactly 2 nodes inside** — those become A and B in one
  gesture, with auto-save firing if the flag is on.
- **Exactly 1 node inside** — that node becomes A only; the
  measurement is half-complete and waits for a B click.
- **0 nodes** — Curvz toasts "No measurement point at this
  location" at the marquee origin; A/B are left unchanged.
- **More than 2 nodes** — Curvz shows an alert dialog ("Only 2
  nodes can be measured at a time"); A/B are left unchanged.

The 2-node marquee is faster than two clicks when the two
endpoints are clearly isolated within a region — drag a small
box around both, release, done.

## Promote (Enter) and Clear (Space)

Two keyboard verbs supplement the click flow:

- **Enter** — when both A and B are set, **commits the
  measurement** to the Measurements layer (the same auto-save
  that fires on shift-click and marquee completions). Useful
  if your plain-click 2nd-click completed the pair but
  save-on-completion was off at the time and you've since
  decided to keep this one.
- **Space** — clears the current A/B picks. The tool stays
  active; the next click starts fresh.

## What the labels show

A completed measurement draws a triangle between A and B with
several on-canvas labels:

- **Distance** — at the midpoint of the A→B hypotenuse, in the
  document's display unit.
- **Δx** — along the horizontal leg, the X-axis distance from
  A to B (always positive — magnitude only).
- **Δy** — along the vertical leg, the Y-axis distance from A
  to B.
- **α at A** — the interior angle at A in degrees.
- **β at B** — the interior angle at B in degrees (α + β = 90°,
  since the triangle has a right angle at the C corner).
- **(x, y) at A** and **(x, y) at B** — small coordinate pills
  pinned to each endpoint, in document units.

The triangle is drawn with the hypotenuse in blue and the two
legs in dashed green, with a small right-angle box at the C
corner where the legs meet.

## Click-to-copy

Clicking any of the labels **copies the full structured
measurement data** to the system clipboard. Format is a
multi-line text block:

```
x₁ = …,  y₁ = …
x₂ = …,  y₂ = …
ΔX = …,  ΔY = …
Distance = …
Angle = …°,  α = …°,  β = …°
```

Every label copies the same block — so you don't have to hit
the "right" one. The block reports the full angle (CCW from +X
axis, 0–360°) alongside α and β; the on-canvas labels show only
α and β to keep the overlay readable.

A small "Copied measurement data" toast appears briefly above
the label to confirm.

## Save versus transient

What happens after you complete a measurement depends on the
**Measure** tool button's right-click popover settings:

- **Save measurements ON**: every completion (plain 2nd-click,
  shift-click, marquee, or Enter on a click pair) appends to
  the document's Measurements layer. Persists across save/load.
- **Save measurements OFF** (default): the measurement is
  transient — visible until you dismiss it, but not part of the
  document. **Delete on copy** controls whether copying the
  label dismisses the transient measurement.

Saved measurements are real document objects on the
**Measurements** layer. Delete them via the Layers panel × button.

## Where to next

For the **measurement preferences** (save versus transient,
delete on copy), **right-click the Measure tool button** —
both toggles live in the popover. These used to live in a
Measure inspector section; that section was retired when the
toggles moved to the toolbar popover.

For static **rulers** along the canvas edges (the coordinate
strips at the top and left), see **Canvas, rulers & corner**
(3.6). The canvas-edge rulers and the Measure tool are
distinct: the rulers are persistent coordinate references
along the workspace edges, the Measure tool is for explicit
distance/angle readouts between two points.

If you find yourself measuring the same distances repeatedly,
turn save mode on and the measurements live in the document as
permanent annotations. Useful for icon design specs that need to
travel with the file.
