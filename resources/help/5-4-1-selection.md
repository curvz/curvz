# Selection

The **Selection** section is the standard inspector view for one or
more selected objects. It shows where the selection sits, how big it
is, and bundles the everyday transforms — scale, rotate, skew, flip
— in a single pane.

For most node kinds — paths, groups, compound paths, clip groups,
blends — Selection is the section you reach for first when you want
to position something precisely. Reference points show a stripped-
down version with just X/Y; image and text nodes have their own
specialised replacements.

![The Selection section showing POS, SIZE, transform rows, and Flip buttons](img/5-4-1-selection-section.png)

## ❶ POS and SIZE

The top of the section is a small grid of four numeric spin
buttons:

- **POS** row — **X** and **Y** of the selection's origin.
- **SIZE** row — **WIDTH** and **HEIGHT** of the bounding box.

The origin convention follows Curvz's Y-up coordinate system: X is
the left edge of the bounding box, Y is the top edge in screen
terms. A move-to-zero on both takes the selection's top-left to the
origin marker (0, 0). Editing W or H scales the selection from a
fixed corner — width grows from the left edge, height from the
bottom edge.

All four spinners respect the document's display unit (set in the
**Canvas** section, 5.3.2). They commit on Enter or focus loss and
edits coalesce — a quick run of nudges or arrow-presses collapses
into a single Ctrl+Z.

### Visual size readout

When the selection has a stroke, four secondary read-only rows
appear under POS and SIZE labelled **Xv / Yv / Wv / Hv**. These are
the **visual** position and size — the bounding box that includes
the stroke's outer extent. If your icon needs to fit in a box that
includes the stroke (most do), watch these rather than the
construction-only POS/SIZE values above.

The visual rows hide when the selection has no stroke contribution
(stroke type None, or width 0). In that case construction and
visual sizes are identical, so the readout is redundant.

## ❷ SCALE

A row with two percent spinners and a **link** toggle:

- Type a percent into either spinner and press Enter (or click the
  **SCALE** label) to apply that scale relative to the bounding-box
  centre.
- The link toggle (chain icon between the two spinners) couples X
  and Y so a value typed in either applies to both. Tap the chain
  to unlink for non-uniform scaling.
- After applying, the spinners reset to 100% — they are an
  incremental scale, not an absolute one.

The bounding-box centre is the pivot. To scale around a different
point, move the selection first so the desired pivot lands on the
bbox centre, scale, then move back.

## ❸ ROTATE

A single spinner from -360 to 360 degrees. Type a value and press
Enter (or click the **ROTATE** label). Rotation is around the
bounding-box centre. After applying, the spinner resets to zero.

## ❹ SKEW

Two spinners — **▐X** (skew along X, left edge fixed) and **▄Y**
(skew along Y, bottom edge fixed). Each accepts -89° to 89°. Type a
value and press Enter (or click the **SKEW** label). After
applying, the spinners reset to zero.

The pivot for X-skew is the left edge of the bounding box; for
Y-skew it's the bottom edge. The two indicator glyphs to the left
of each spinner show which edge stays put.

## ❺ Flip H / Flip V

Two buttons at the bottom of the section:

- **⇔ Flip H** mirrors the selection horizontally about the
  bounding-box centre.
- **⇕ Flip V** mirrors vertically.

Both work for any selection — paths, groups, refs, multi-object —
and push undo as one step.

## What changes for special node types

Selection is not always titled "Selection". The same code path
substitutes a different layout when the primary selection is
something other than a regular path or group:

- **Reference points** show a small Selection section with only
  POS (X / Y) — no size, since refs are points. Below it is the
  same Flip H / Flip V pair, applied to the ref's neighbourhood.
- **Image nodes** show an Image-specialised Selection section with
  X / Y / W / H plus image-specific properties (crop, rotation,
  source path).
- **Text nodes** replace Selection entirely with a **Text** section
  (5.4.2) — content, font, size, alignment, baseline, letter
  spacing, optional path linking. The Selection section header
  does not appear at all.

Multi-object selections always use the standard layout above; the
specialised forms only fire for a single node of that type.

## Empty state

With nothing selected, the Selection section header still appears
but the body is empty. The Document group's controls (Canvas,
Guides, Grid, Margins, Snap, Measure) are what apply when you have
nothing on the canvas to inspect.

## Where to next

- **Node** (5.4.4) — when a single node is selected, the inspector
  shows its anchor, handles, and type. Use it for precision Bézier
  editing.
- **Appearance** (5.4.5) — fill, stroke, and opacity controls.
- **Shadow** (5.4.6) — drop shadow, conditional on the selected
  type supporting one.

If you do most of your alignment work via Selection's POS row, you
will want **Snap** (5.3.6) configured — guides + nodes are usually
the right defaults.

### Keys

These bindings only fire when the canvas has focus and the
Selection tool is active.

- `←` / `→` / `↑` / `↓` — nudge the selection. Default step is
  about 2 screen pixels (in document units, scaled by zoom).
- `Shift + arrow` — medium nudge, about 8 screen pixels.
- `Alt + arrow` — large nudge, about 32 screen pixels.
- `Ctrl + arrow` — reserved for arrangement actions; not used as
  nudge.
- `Delete` / `Backspace` — delete the selected object. Guides are
  prioritised: if any guides are selected, Delete removes those
  first.

If a spin button has focus instead of the canvas, arrow keys nudge
the spinner's value, not the selection. Click the canvas once or
press **Esc** to return focus before nudging.
