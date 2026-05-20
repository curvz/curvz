# ![Eyedropper tool icon](img/icons/curvz-eyedropper-symbolic.svg) Eyedropper

The **Eyedropper tool** samples a colour from anywhere on the
canvas and applies it to the current selection. It's a one-shot
tool: pick a colour with a click, and Curvz returns you to
whichever tool you were using before.

Activate it from the toolbox or with the **I** key (canvas focus
required). When active, the cursor shows a magnifying loupe
overlay that follows your hover position.

![The Eyedropper loupe over the canvas, showing the magnified pixel grid at the cursor](img/4-6-1-eyedropper-sample.png)

## The loupe

The Eyedropper has an **always-on magnified preview** — a small
loupe (a circular zoomed-in view) follows the cursor as you
hover. The loupe shows the canvas pixels around the cursor at a
significantly higher zoom than the document is currently set to.
A crosshair in the centre marks the exact pixel that will be
sampled on click.

The loupe is most useful for:

- Sampling a colour from a small icon detail that's hard to hit
  precisely at the document's current zoom.
- Picking a specific pixel within a gradient or near an edge,
  where neighbouring pixels carry different colours.
- Confirming you're sampling from the right place before
  committing.

The loupe is always-on (since Curvz s66) — there's no toggle to
turn it off; the magnification is part of the tool's
identity. If the loupe feels in the way for a particular pick,
just click — the loupe vanishes the moment the sample commits.

## What clicks do

A click samples the centre of the loupe and applies the result:

- **Click** — applies the sampled colour to the **fill** of every
  selected object.
- **Alt + click** — applies to the **stroke** instead of the
  fill. The cursor and loupe are otherwise identical; only the
  destination channel changes.

After commit, Curvz returns to **whatever tool was active before
the Eyedropper** — Selection, Pen, Node, whichever. The
Eyedropper is meant to be a quick interruption, not a destination.

## What gets sampled

The sample is taken from the **rendered canvas**, not from any
specific underlying object. That means:

- If a path is in front of another path, the Eyedropper picks
  whatever pixel is visible at that point — including any
  blending, transparency, or anti-aliasing edges.
- Sampling on a stroke samples the stroke's painted pixel, not
  the stroke style (no width, no cap/join — just the colour).
- Sampling on a drop shadow samples the rendered shadow pixel,
  which is often a tinted, partially-transparent version of the
  underlying colour.

The sampled value is a **solid RGBA colour**. If the underlying
artwork was painted with a gradient, the picked colour is just
the specific point along that gradient where the cursor was —
not a copy of the gradient itself. To copy a gradient, pick the
artwork itself with the Selection tool and copy its appearance.

## Multi-object selection

When more than one object is selected at the moment of pick, the
sampled colour applies to **every selected object** as a single
composite undo step. Ctrl+Z reverts the whole batch.

This is useful for harmonising appearance across a set —
multi-select a group of related shapes, eyedropper-pick from a
reference object, and they all match in one gesture.

## Bindings break on pick

Like every other appearance edit, an Eyedropper pick is a **user
override**, so any swatch or style binding on the targeted
object is dropped as a side effect. The colour stays where you
sampled it; the link to the named asset library does not.

If you want to *update the swatch* rather than break its link,
edit the swatch in the **Swatches** panel (6.4) instead — every
object bound to that swatch updates live, and bindings stay
intact.

## What about a hex copy?

There's no built-in "copy hex value" gesture on the Eyedropper.
After sampling, the colour ends up in the toolbar's fill or
stroke well — open the **Color picker** (3.7) from the well to
see the hex value and copy it from there.

A future version of Curvz may add an Alt-modifier pick that
copies hex without applying; today that workflow is "pick →
open picker → read hex".

## Where to next

The destination of an Eyedropper pick is the **Appearance**
inspector section (5.4.5), where the new colour shows in the
fill or stroke paint editor. Open the Color picker (3.7) from
there to fine-tune the sampled colour after the fact.

The companion utility tools are **Zoom** (4.6.2) and **Measure**
(4.6.3) — together they cover the three tools that don't draw
new artwork but support every other aspect of the workflow.

For sampling colours from outside Curvz (a screenshot, a
reference photo), import the image into the document first
(File → Import) and Eyedropper-pick from the placed image.
