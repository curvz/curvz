# Boolean operations

Boolean operations combine paths into one new path by geometric
set logic — keep the area covered by every input, by any input,
or by one minus the others. **Union**, **Subtract**, and
**Intersect** are the three operations Curvz offers. They live
under the **Path** menu and as Ctrl+Shift hotkeys.

The result is a single new path that *replaces* the inputs.
Boolean ops change geometry permanently, unlike clip masks which
only hide regions — Ctrl+Z reverts to the originals if you change
your mind, but the operation itself doesn't keep the sources
around.

![Two overlapping shapes, before and after Union](img/8-2-boolean-result.png)

## ❶ Requirements

To run any boolean op you need **two or more Path objects** selected.
Select the paths and trigger the operation; if you have fewer than
two valid candidates the menu items dim out.

A few practical points:

- The selected paths must be **closed**. Open paths are skipped
  with a notice — they have no enclosed area for the set logic to
  work on. Close them first, or convert to filled regions, before
  running a boolean op.
- **Compound paths** (paths with holes — see **Compound & derived**,
  8.3) work correctly as boolean inputs in the common case of
  *one compound combined with one simple path*. Curvz decomposes
  the compound by its child stacking order — the bottom-most
  child is treated as the outer outline; children stacked above
  alternate hole, island, hole, and so on — and runs the boolean
  per role against the path operand, so outer-minus-hole
  semantics survive the operation. For *two or more compounds*
  in one selection, or for selections of three or more inputs
  with any compounds in them, the result still falls through to
  Clipper2's general N-way path and the topology may not be what
  you'd expect. In those cases release the compound(s) first
  (right-click → Release Compound, or `Ctrl + Shift + 8`), run
  the boolean on the simple paths, and re-form the compound if
  needed.

## ❷ The three operations

**Union** ❷ keeps every region covered by **any** of the inputs.
The result is the outline that wraps all the shapes — useful for
joining overlapping forms into a single mass without seams.

**Subtract** removes every other input's region from the
bottommost (lowest-z) input. Where shapes overlap, the result has
the bottom shape's geometry minus those overlaps. Order matters:
the bottommost path in z-order is treated as the base, and the
rest are cutters.

**Intersect** keeps only the region covered by **every** input —
the area where they all overlap. Anywhere not covered by all
inputs gets discarded. This is how you mask a shape down to the
part of itself that's inside another shape (or several others),
with the result a permanent path rather than a clip.

## ❸ Triggering an operation

Three ways to fire a boolean op:

- **Path menu** — Union / Subtract / Intersect at the top of the
  menu.
- **Hotkeys** — `Ctrl + Shift + U` Union, `Ctrl + Shift + E`
  Subtract (E for *exclude*), `Ctrl + Shift + I` Intersect.
- **Right-click** on the canvas with a valid selection of two or
  more closed paths. The three ops live under a **Boolean**
  submenu on the context menu, sitting between the Structure
  section (Group / Compound) and Arrange.

The menu items and the hotkeys consult the same enable/disable
state. If a hotkey doesn't fire, check the menu — the item will
be dim, which means the current selection doesn't satisfy the op
(needs two or more closed Path objects). When an op *does* fire
but the inputs filter down to fewer than two closed paths
(because some were open or weren't paths), Curvz shows a
"Boolean Operation Failed" notice with a count of what was
skipped.

## ❹ The result

After a successful operation:

- A **single new path** replaces the originals at the lowest-z
  position among the inputs (so the result lands where the
  back-most shape was, in render order).
- The result inherits the **bottommost (lowest-z) input's
  appearance** — fill, stroke, dash. Other inputs' appearances
  are discarded. (For a Compound input, the appearance comes from
  its topmost child — Compound containers don't carry their own
  fill or stroke.)
- The **node types** of the result are recorded on a
  `data-curvz-types` SVG attribute so the path round-trips
  through save and load with its node classifications intact.

The whole operation is one undo step. Ctrl+Z restores the
original paths exactly as they were, including bindings to
swatches or styles.

## ❺ Cleaning the output

The clipping engine produces topologically-correct results, but
the curves it emits are flattened into many short polyline-style
segments. A union of two circles can come back with **hundreds of
nodes** describing what should be a smooth boundary. The geometry
is right; the editing experience isn't.

The **Boolean cleanup** subsection in the inspector's
**Application** group controls a post-pass that walks the result
and reduces it to the smaller node set Curvz can recognize: the
original input anchors that survived the operation, the points
where the input curves crossed each other, and a one-node guard
band on either side of each of those to keep the curve shape
stable. Everything else gets removed, and the curve flows through
what remains.

The control is a single slider, range 0 to 10. Lower values
favour fewer nodes at the cost of shape fidelity; higher values
keep more nodes for a tighter match. The end labels read
**Approximate (fewer nodes)** and **Faithful (more nodes)**. The
default sits in the middle at 5; the right-most stop, labelled
**Raw**, turns the post-pass off entirely and hands back the
unprocessed Clipper2 output. Curvz remembers the slider position
across launches.

A worked example: a rectangle plus a circle, unioned. At the Raw
end (10) the result is around 140 nodes, all straight cusps
tracing a polyline approximation of the circle. At the default
(5) the result drops to around 15 nodes — the rectangle's
corners, the circle's symmetric anchors, the two points where
the circle crossed the rectangle's right edge, and the guard
band protecting them. The curve flows through every keeper as a
clean cusp; the segments between keepers carry the curvature.

**The result is a starting point, not a finished shape.** Curvz
hands you a clean, editable boundary; you take it the rest of
the way using the existing single-node-delete tool (select node,
press Delete). Curvz's delete renegotiates the neighbouring
handles when you remove a node, so each delete preserves the
shape locally and you can polish at your own pace. The cleanup
gets you most of the way; your eye finishes it.

## ❻ Current limits

The boolean implementation is backed by Clipper2 (Angus Johnson's
robust polygon clipping library) and handles two-or-more closed
paths reliably for all three operations.

A few caveats to be aware of.

**Compound paths.** Compound + simple Path works correctly via
per-child role dispatch — outer-minus-hole topology survives the
operation. Compound + Compound, or three-or-more inputs with any
compounds in the mix, falls through to the general Clipper2 path
and the result may not preserve the source compound's hole
structure. Release the compound(s) first, run the operation on
the simple paths, then re-form the compound if needed. See ❶ for
the release shortcut.

**Small canvases produce coarse curve output.** Boolean operations
work at the canvas's native resolution. On small canvases —
especially Pixel-mode 16, 24, or 32 — curves are represented with
relatively few segments, and the boolean output reflects that
resolution. A union of two circles on a 16-px canvas comes back
visibly faceted, not because the math is wrong but because there
isn't enough room in the geometry for smooth curves to begin with.
If you need smooth boolean output at small final sizes, author at
a larger Quality (Print, Poster) and scale down once the booleans
are run, or work in Ratio or Physical mode where curve smoothness
is preserved regardless of canvas dimensions. In Pixel mode the
coarse output is the contract — the canvas committed to a pixel
grid, and the boolean honors it.

**Cleanup keeps a guard band.** Anywhere the slider sits below
the Raw end, the post-pass holds onto the immediate neighbours
of every keeper anchor — one node on each side, regardless of
whether that node carries useful curvature. The guard makes the
result robust on adversarial inputs but means the node count is
typically higher than an optimal hand-cleanup of the same shape.
Expect 2–3× the keeper count in practice. If you want fewer
nodes than Curvz produced, delete the redundant ones by hand —
Curvz's single-node-delete renegotiates the neighbouring handles
to preserve the shape locally, so you can keep deleting until
the path looks right.

**Cleanup at intersections may carry a little overshoot.** Where
two input curves cross, the post-pass keeps the renegotiated
handles produced by the cleanup walk on either side of the
intersection rather than retracting them. On most geometries
this looks correct; on adversarial inputs (very different
curvatures meeting at a sharp angle) the curve can bulge slightly
past the original boundary near the cusp. If you see overshoot,
delete the cusp by hand and re-add a node where you'd prefer the
curve to pin — or slide the Boolean cleanup control to Raw,
re-run the operation to see the unprocessed shape, and pick the
result that suits.

Open paths are **skipped** rather than producing wrong geometry —
if your selection includes some open paths, the op proceeds on
the closed remainder. If fewer than two closed paths remain after
filtering, the operation aborts and Curvz shows a "Boolean
Operation Failed" notice with a count of what was skipped.

## When to use which

Some everyday patterns:

- **Combining a body and an extension** — say, a square base with
  a pointed roof. Union turns them into one outline you can then
  style and stroke as a single shape.
- **Cutting out a notch** — draw the cutter shape on top of the
  target, select both, Subtract. The bottom shape (the target)
  keeps its fill and gets the cutter's region removed.
- **Trimming to a frame** — draw the trim shape (a circle, say)
  on top of the artwork, select both, Intersect. The result is
  the artwork cropped to the trim. Useful when you want a cropped
  *copy* without the masking overhead of a ClipGroup.

For non-destructive cropping where the underlying shape stays
editable, prefer **Clip masks** (7.2) over Intersect / Subtract.

## Where to next

- For paths that *contain holes* (the donut pattern), see
  **Compound & derived** (8.3) — compound paths are a different
  approach to multi-region geometry.
- For non-destructive cropping, see **Clip masks** (7.2).
- For a single path's structural edits (open / close / split /
  join), see **Editing paths** (8.1).
- To grow or shrink a path's outline rather than combine it with
  another, see Offset Path under **Compound & derived** (8.3).
