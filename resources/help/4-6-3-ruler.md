# ![Ruler tool icon](img/icons/curvz-ruler-symbolic.svg) Ruler

The **Ruler tool** measures distance and angle between two points
on the canvas. It picks **node and reference-point endpoints** as
its anchor positions — measurements snap to existing artwork, so
you get exact distances rather than eyeballed ones.

Activate it from the toolbox or with the **M** key (canvas focus
required).

For the Ruler tool's behaviour preferences (save measurements,
delete on copy), see the **Measure** inspector section (5.3.7).
Those preferences govern what happens *after* you complete a
measurement; this tool covers the picking flow.

![A completed measurement showing two endpoint markers and the labelled triangle between them](img/4-6-3-ruler-measurement.png)

## Picking endpoints

A measurement is two endpoints: **A** and **B**. The clicks pick
them:

- **Click on a node or refpt** within 8 pixels — sets that as **A**
  and clears any previous measurement.
- **Shift + click on a different node or refpt** — promotes the
  current A to **B** and sets the clicked one as the new A.
  Confusingly worded but useful: the *most recent click is always
  A*, the previous A becomes B, so a measurement reads forward.

Once both A and B are set, Curvz draws the measurement triangle
between them with three labels — distance, horizontal Δx, and
vertical Δy.

The 8-pixel pick tolerance is in screen space, so it scales with
zoom. Zoom in for fine work; zoom out for forgiving snaps over
larger distances.

## Marquee shortcut

Drag a **marquee** on empty canvas to box-select **exactly two
nodes**. If the marquee contains exactly two nodes, those
become A and B; if it contains zero or more than two, the
measurement clears.

This is faster than two clicks when the two endpoints are clearly
isolated within a region — drag a small box around both, release,
done.

## Promote (Enter) and Clear (Space)

Two keyboard verbs supplement the click flow:

- **Enter** — when both A and B are set, **commits the
  measurement** (places it on the canvas as a saved annotation,
  if save-on; otherwise leaves it as a transient overlay).
  Exactly the same as the auto-save that fires when shift-click
  completes the pair.
- **Space** — clears the current A/B picks. The tool stays
  active; the next click starts fresh.

The Enter verb is what you reach for if you've built up a
measurement via two plain clicks (no shift) and want it saved —
plain clicks don't auto-save by themselves, only shift-click
completions do.

## What the labels show

Three labels appear on the measurement triangle:

- The **hypotenuse label** at the midpoint of the A-B line shows
  **distance** (in the document's display unit).
- The **horizontal label** along the bottom of the triangle shows
  **Δx** — the X-axis distance from A to B.
- The **vertical label** along the side shows **Δy** — the
  Y-axis distance.

A small arrow on the line marks direction (A to B).

In addition to the three labels, two **angle annotations** show
the slope angle relative to horizontal and the complement
relative to vertical, in degrees. They're useful for measuring
"is this perfectly horizontal" or "what angle does this slope
sit at."

## Click-to-copy

Clicking any of the labels **copies the full structured
measurement data** to the system clipboard. Format is a
multi-line text block:

```
A: x1, y1
B: x2, y2
Δ: dx, dy
distance: d
angle: θ°
```

A small "Copied measurement data" toast appears briefly above the
label to confirm.

The same copy block goes regardless of which label you clicked —
so you don't have to hit the right one. Click distance, click
Δx, click Δy: same data either way.

## Save versus transient

What happens after you complete a measurement depends on the
**Measure** inspector section's settings (5.3.7):

- **Save measurements ON**: every completion (shift-click pair
  or Enter on a click pair) appends to the document's
  Measurements layer. Persists across save/load.
- **Save measurements OFF** (default): the measurement is
  transient — visible until you dismiss it, but not part of the
  document. **Delete on copy** controls whether copying the
  label dismisses the transient measurement.

Saved measurements are real document objects on the
**Measurements** layer. Delete them via Layers panel × button
or the inspector's per-measurement controls.

## Where to next

For the **measurement preferences** (save versus transient,
delete on copy), see the **Measure** inspector section (5.3.7).

For static **rulers** along the canvas edges (the coordinate
strips at the top and left), see **Canvas, rulers & corner**
(3.6). The rulers and the Ruler tool share a name but cover
different needs — the rulers are persistent coordinate
references, the tool is for explicit measurements.

If you find yourself measuring the same distances repeatedly,
turn save mode on and the measurements live in the document as
permanent annotations. Useful for icon design specs that need to
travel with the file.
