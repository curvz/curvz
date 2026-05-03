# Boolean operations

Boolean operations combine two paths into one new path by
geometric set logic — keep the area covered by both, by either,
or by one but not the other. **Union**, **Subtract**, and
**Intersect** are the three operations Curvz offers. They live
under the **Path** menu and as Ctrl+Shift hotkeys.

The result is a single new path that *replaces* the two inputs.
Boolean ops change geometry permanently, unlike clip masks which
only hide regions — Ctrl+Z reverts to the original two paths if
you change your mind, but the operation itself doesn't keep the
sources around.

![Two overlapping shapes, before and after Union](img/8-2-boolean-result.png)

## ❶ Requirements

To run any boolean op you need exactly **two Path objects**
selected. Select the two paths and trigger the operation; if you
have one path or three the menu items dim out.

A few practical points:

- The two paths should be **closed**. Open paths can be selected
  but the geometry results are unreliable — the operation needs
  filled regions to compute set membership.
- Compound paths (paths with holes — see **Compound & derived**,
  8.3) are not yet supported as boolean inputs in this build.
  Convert a compound back to its parts, run the boolean, and
  re-compound if needed.

## ❷ The three operations

**Union** ❷ keeps every region covered by **either** input. The
result is the outline that wraps both shapes — useful for joining
two overlapping forms into a single mass without the seam.

**Subtract** removes the second path's region from the first.
Where the two overlap, the result has the first path's shape
minus that overlap. Order matters: which path is "first" depends
on selection order in the document — earlier in the layer's
z-order is treated as the base, the later one as the cutter.

**Intersect** keeps only the region covered by **both** inputs —
the overlap. Anywhere only one shape covers gets discarded. This
is how you mask a shape down to the part of itself that's inside
another shape, with the result a permanent path rather than a
clip.

## ❸ Triggering an operation

Three ways to fire a boolean op:

- **Path menu** — Union / Subtract / Intersect at the top of the
  menu.
- **Hotkeys** — `Ctrl + Shift + U` Union, `Ctrl + Shift + E`
  Subtract (E for *exclude*), `Ctrl + Shift + I` Intersect.
- **Right-click** on the canvas with a valid two-path selection.

The menu items and the hotkeys consult the same enable/disable
state. If a hotkey doesn't fire, check the menu — the item will
be dim, and the status bar will explain why (wrong selection
size, wrong types, open paths).

## ❹ The result

After a successful operation:

- A **single new path** replaces the two originals at the lower-z
  position of the inputs (so the result lands where the back
  shape was, in render order).
- The result inherits the **first selected path's appearance** —
  fill, stroke, dash. The second input's appearance is
  discarded.
- The **node types** of the result are recorded on a
  `data-curvz-types` SVG attribute so the path round-trips
  through save and load with its node classifications intact.

The whole operation is one undo step. Ctrl+Z restores the two
originals exactly as they were, including bindings to swatches
or styles.

## ❺ Current limits

The boolean implementation in this build uses Bézier clipping
(Sederberg–Nishita) and is correct for most common cases but has
known limits:

- **Union of three or more paths** in one go can show artefacts.
  If you need to union several paths, do it pairwise: Union the
  first two, select the result with the third, Union again.
- **Subtract and Intersect** are less battle-tested than Union.
  Verify the result on first use; if you see artefacts, undo and
  try a small geometric tweak (a fractional-pixel nudge to one
  input often resolves edge-case clip failures).
- **Compound-path inputs** are not yet supported as documented
  above.
- **Open paths** are accepted but produce unreliable geometry.
  Close them first or convert to filled regions.

These are tracked for fixes in upcoming releases. If you hit a
case that the operation gets visibly wrong, undo, save the
project under a different name, and the source files will help
reproduce the bug for fixing.

## When to use which

Some everyday patterns:

- **Combining a body and an extension** — say, a square base with
  a pointed roof. Union turns the two into one outline you can
  then style and stroke as a single shape.
- **Cutting out a notch** — draw a rectangle that overlaps the
  shape where you want the bite, select the target then the
  cutter, Subtract.
- **Trimming to a frame** — draw the trim shape (a circle, say),
  select it after the artwork shape, Intersect. Useful when you
  want a cropped *copy* without the masking overhead of a
  ClipGroup.

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
