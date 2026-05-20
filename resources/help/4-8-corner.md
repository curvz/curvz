# ![Corner tool icon](img/icons/curvz-corner-symbolic.svg) Corner

The **Corner tool** applies corner treatments — **rounding**,
**chamfering**, and **inverse rounding** — to selected anchor
nodes. It's a node-modifier tool, not a shape creator: pick the
nodes whose corners you want to treat, choose a treatment, set a
radius, click Apply.

Activate it from the toolbox or with the **K** key (canvas focus
required). On the toolbar Corner sits in the Transforms section
alongside Align, Blend, Bool, Step and Repeat, and Warp — it's
shaped like a tool toggle, but conceptually it modifies geometry
in place, which is why it lives with the other geometry-modifying
verbs rather than with Pen and the shape tools.

![Three corners on the same path: round, chamfer, and inverse-round at the same radius](img/4-8-corner-treatments.png)

## Picking nodes

When the Corner tool is active, the canvas highlights every
**Corner-** and **Cusp-type node** on every visible path — those
are the nodes the treatment can act on. Smooth and Symmetric
nodes are intentionally excluded; they have curvature already
defined by their handles, so a corner treatment on them would
be ambiguous.

### Click selection

- **Click a Corner/Cusp node** — selects just that node (clears
  any previous selection unless that node was already selected).
- **Shift + click** — toggles the node in or out of the
  multi-selection. The selection persists across clicks.
- **Drag a marquee on empty canvas** — selects every Corner/Cusp
  node inside the rectangle. Combine with Shift to add to an
  existing selection.

The selection count appears in the **Context bar** at the top of
the canvas — useful for checking that you've picked the right
number before applying.

## The Corner panel

The Corner tool's controls live in a **popover anchored to the
Corner toolbar button**, opening to the right of the toolbar
the first time a non-empty corner selection exists. Once open
the panel is **sticky** — every click on the canvas (each
selection change) keeps the panel up rather than dismissing it,
which would be unusable for a tool whose whole flow is
"select-some-nodes, set-radius, Apply, select-different-nodes,
Apply again."

Inside the popover:

- **Round / Chamfer / Inverse** — three toggle buttons in a
  radio group, exactly one active at a time. Picks the
  treatment type. Round is selected on every open.
- **Radius** — a numeric entry accepting a distance in the
  document's display unit (label says "Units: …" with the
  current unit) or an expression like `8px`, `0.125in`, or
  `3px + 2mm`. Defaults to 0.1; set to whatever the treatment
  needs. Pressing **Enter** in the radius entry is equivalent
  to clicking Apply.
- **Apply** button — commits the treatment to the current node
  selection and **dismisses the popover**. The op completes
  fully; the next selection will re-open the panel.

The panel dismisses on:

- **Apply** (or Enter in the radius entry) — the op completes.
- **Escape** — wired explicitly because the panel's sticky
  mode bypasses GTK's automatic Escape routing.
- An explicit click on the Corner toolbar button to deactivate
  the tool.

It does **not** dismiss on outside click, which is the whole
point of the sticky behaviour — clicks on the canvas are
selection edits, not panel-cancel gestures.

## The three treatments

### Round

A **cubic Bézier arc** approximating a circular fillet between
the two segments meeting at the corner. The handles are computed
to give a tangent-continuous arc — no kink at the start or end of
the rounded section.

Most common treatment for icon work. Use larger radii for soft,
friendly icon styles; smaller radii for subtle softening that
still reads as a "corner" rather than a curve.

### Chamfer

A **straight cut** across the corner — the angled segment that
replaces the sharp point. Two new straight segments meet at a
flat angle, with the bevel between them.

Use for technical or industrial visual styles. Chamfers read as
"bevelled edges" rather than "softened corners," even at the
same notional radius.

### Inverse Round

A **concave arc** — the same shape as Round, but with the
handles flipped to bow inward instead of outward. Produces a
scooped-out corner.

Less common than Round and Chamfer; useful for negative-space
icons (a rounded notch cut out of a shape) and for stylised
ornamentation.

## What gets skipped

The treatment skips nodes silently when:

- The node is **not Corner or Cusp** (Smooth and Symmetric
  excluded).
- The node's **angle is below 15°** (treated as too sharp; a
  treatment at a sharp angle would overshoot the segment).
- The angle is **above 170°** (treated as too flat; the corner
  is effectively a straight line, no treatment needed).
- An adjacent segment has a **non-degenerate handle** (the
  incoming or outgoing segment is already curved). Curvz
  treatments only work on straight-meets-straight corners.
- The node is an **endpoint of an open path** with no segment
  on one side.
- Either adjacent segment is **degenerate** (zero length).

If a multi-node selection includes some treatable and some
untreatable corners, only the treatable ones change; the rest
are left alone.

## Radius clamping

The radius is **clamped per-node** to whatever the local
geometry can support. If you ask for a radius of 20 px on a
corner whose adjacent segments are only 10 px long each, the
treatment will use 5 px instead — the cap is half the
shorter-adjacent-segment length so the treatment doesn't
overshoot the midpoint.

This means a single Apply with one radius value can produce
different *visible* radii at different corners, depending on
how much room each corner has. For uniform visible radii across
a polygon, all the segments need to be at least 2× the radius
long.

## After Apply

The treatment **bakes** into the path. The path's geometry
changes — the modified corner is now described by two anchor
nodes (P1 and P2) replacing the original corner node, with
Bézier handles for Round and Inverse Round, or with a straight
segment between them for Chamfer.

Once baked, the corner is no longer "parametric" — Curvz does not
remember the old sharp version. You can:

- **Ctrl+Z** to undo immediately, restoring the sharp corner.
- **Apply again with a different radius** to overwrite the
  current treatment with a new one. Curvz does not detect that
  the corner is already treated; it just applies the operation
  to the current geometry, which is rarely what you want for
  Round-on-Round. Better to undo first.

## Where to next

Corner pairs naturally with **Polygon** (4.4.4) — draw a star,
round its corners, done. The two-tool workflow is one of the
most common icon-creation paths in Curvz.

For *node-level* edits more granular than corner treatments, the
**Node tool** (4.2.2) is the right choice — it lets you drag
handles individually for non-uniform shaping. The Corner tool
is parametric; the Node tool is freeform.

Corner treatments don't have a counterpart in the inspector —
there's no per-node "current corner radius" property because
the result is just baked path geometry. After applying, the
treated corners are indistinguishable from any other curves you
might have drawn freehand.
