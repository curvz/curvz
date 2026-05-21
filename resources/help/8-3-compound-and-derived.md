# Compound & derived

A handful of operations don't combine paths or modify them in
place — they *derive* new path geometry from existing artwork.
Compound paths bundle multiple outlines into one fill region
(letting you cut holes). Offset Path grows or shrinks an outline
outward. Expand Stroke turns a stroked line into a filled outline.
Convert Text to Path turns text into editable curves.

These all share a shape: take what you have, produce a new path
based on it, leave you with a single result you can keep editing.
This section walks each one in turn.

![A compound path beside its source outlines](img/8-3-compound-path.png)

## ❶ Compound paths

A **compound path** is a single path object built from multiple
outlines treated as one fillable region. The classic case is a
donut: one outer circle plus one inner circle, the inner marked
as a hole, filled together so only the ring is painted.

Compounds use the **even/odd fill rule**: at any point, count how
many outlines wrap around it. Odd → painted; even → hole. Two
nested outlines produce a ring. Three nested produce a ring with
a disc inside. Four nested produce a ring with a ring inside.
Curvz computes which-is-which automatically from the geometry —
you don't tag outlines as "outer" or "hole" by hand.

### Making a compound

Select two or more closed paths and choose **Path → Make Compound
Path**, or press `Ctrl + 8`. The selected paths merge into one
compound object that takes the appearance (fill, stroke, dash) of
the **bottom-most** path in the selection. The other paths' fills
are discarded — the compound has one fill, not many.

In the Layers panel a compound shows as a single row. The
constituent outlines are still individually editable: pick their
nodes directly with the Node tool (which descends into the
compound's children and hits the closest leaf path), or expand
the compound's row in the Layers panel and click a member to
select it as the active path before editing.

### Splitting a compound

Select a compound and choose **Path → Split Compound Path**, or
press `Ctrl + Shift + 8`. The compound dissolves and its members
return as separate path objects. Each gets the compound's
appearance — Curvz doesn't try to remember per-path fills from
before the merge.

Both operations are undoable as single steps.

### Compound caveats

- Compounds need **closed paths** to work as fillable regions.
  Open paths in a compound paint with the even/odd rule for any
  edge crossings, which usually isn't what you want.
- **Boolean operations** (Union / Subtract / Intersect) accept a
  compound as one operand against a simple path — Curvz uses
  per-child role dispatch so outer-minus-hole topology survives.
  For *two or more compounds* in one boolean selection, or for
  selections of three or more inputs with any compounds in them,
  the topology may not survive — release first (`Ctrl + Shift + 8`),
  boolean the simple paths, re-compound. See **Boolean operations**
  (8.2) ❶ for the full story.
- The **direction** of each constituent path matters for the
  even/odd computation. If a hole isn't appearing as a hole, try
  reversing that path (`R` with the Node tool, then re-compound).

## ❷ Offset Path

**Offset Path** produces a new path that runs at a constant
distance from an existing one — outward, inward, or both. The
classic uses are mounting margins (a frame around an icon at 4px),
inner shadows (offset inward, then style), and stroke
duplication (offset by stroke width to convert a centred stroke
into a fillable region).

Open the dialog with **Path → Offset Path…** or `Ctrl + Shift + O`
with one or more paths selected.

The dialog has three controls:

- **Distance** — how far from the source, in document units. The
  spinner honours the active display unit.
- **Side** — *Outside* (grow), *Inside* (shrink), or *Both* (an
  outward and inward offset at the same distance, producing the
  band on either side of the source).
- **Keep original path** — leave the source in place and add the
  offset as a new sibling. With it off, the source is replaced.

The result inherits the source's appearance (fill, stroke). When
**Keep original** is on, multi-loop results land as sibling Path
objects above the source — handy when you want to edit each
offset shape separately. When **Keep original** is off, multi-loop
results are wrapped in a **compound path** so the band renders as
one shape under the even/odd rule; release the compound
(`Ctrl + Shift + 8`) if you need the loops apart.

Inside-offsets on tight curves can self-intersect or pinch off
into nothing if the distance exceeds the local radius. Curvz
produces what the math allows; if a result looks degenerate,
reduce the distance.

## ❸ Expand Stroke

**Expand Stroke** turns a stroked path into a *filled* outline of
that stroke. Where you had a 4px stroke running along a centred
line, you end up with a closed path forming the outer edges of
that stroke — which can then be filled, stroked again, used as a
boolean input, or otherwise treated as ordinary geometry.

Trigger it with **Path → Expand Stroke** or `Ctrl + Shift + X`
with one or more stroked paths selected.

The result:

- Replaces the source path with a new path representing the
  stroke's filled outline.
- Inherits the **stroke colour** as the new fill.
- Drops the stroke (the new path renders by fill only).
- Is closed if the source was closed; otherwise it's the closed
  outline of the stroke band.

Useful for taking a calligraphic stroke into a vector glyph,
preparing strokes for boolean operations (which need fillable
regions), or converting variable-width approximations into final
geometry.

The operation respects the source's stroke width, line cap, line
join, and dash pattern. Dashed strokes expand into a sequence of
separate filled blocks, one per dash.

## ❹ Convert Text to Path

**Convert Text to Path** breaks a text node into editable Bézier
outlines and wraps them in a **compound path** — one Path child
per contour, with the even/odd fill rule cutting holes where a
glyph has them (the inside of an "O", the counter of an "a").
After conversion the text is gone: you can't change the wording
or restyle it as text. What you have is path geometry, which can
be edited at the node level, used as boolean inputs, expanded,
offset, and so on.

Open with **Path → Convert Text to Path** or `Ctrl + Alt + T`
with one or more text nodes selected.

This is useful for:

- **Custom glyph tweaks** — converting a single letter to paths
  to redraw a serif or modify a counter.
- **SVG export portability** — paths render the same way
  everywhere; text depends on font availability at render time.
  If your icon needs to read identically on machines that don't
  have your typeface installed, convert first.
- **Boolean inputs** — text becomes valid material for Union /
  Subtract / Intersect once it's path geometry. Because the
  result is a compound, the common one-compound-plus-one-path
  boolean case (see 8.2 ❶) covers it directly.

The operation is undoable as a single step. Once converted, you
can split the resulting compound (`Ctrl + Shift + 8`) if you want
the contours as independent path objects — though for glyphs with
holes this loses the even/odd "this is a hole" relationship and
you'll see the inner contour as a filled region. Keep the
compound unless you specifically need the contours apart.

## Where to next

- For boolean *combinations* of paths, see **Boolean operations**
  (8.2).
- For *cloning* sequences rather than deriving outlines, see
  **Step and Repeat** (7.1).
- For *interpolating* between two paths, see **Blends** (7.3).
- For non-destructive shape masking, see **Clip masks** (7.2).
- For *node-level* editing of the result of any of these
  operations, see **Editing paths** (8.1).
