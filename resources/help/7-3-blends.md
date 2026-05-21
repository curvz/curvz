# ![Blend icon](img/icons/curvz-blend-symbolic.svg) Blends

A **Blend** interpolates between two paths to produce a sequence
of in-between shapes. Pick a start path and an end path, choose a
step count, and Curvz fills the gap with intermediate copies whose
geometry, fill, stroke, and width morph smoothly from one source
to the other.

Blends are how you build tapered strokes (a thick path blended into
a thin one), motion ghosts, lighting falloff, and any pattern
where one shape needs to flow into another. They're the
"variable-width stroke" surrogate in Curvz — make a fat copy and a
thin copy of the same path, blend them, and you have a stroke
that pulses or tapers along its length.

Blend lives in the **Transforms** section of the toolbar alongside
Align, Bool, Corner, Step and Repeat, and Warp — a block of
selection-modifying operations grouped together because they all
act on the current selection rather than on the canvas at large.

![Two source paths with a blend's steps between them](img/7-3-blend-result.png)

## Requirements

To create a Blend you need exactly **two Path nodes** selected,
and they have to share the same parent (same layer or same
group). Compound paths, groups, text, and images are not currently
blendable — only plain paths.

The two paths also need to match on:

- **Closed flag** — both open or both closed. You can't blend an
  open path with a closed one.
- **Node count** — same number of nodes on each side. If they
  don't match, the popover warns and offers to **Equalize** them
  for you (see below).

If either side fails these rules, Curvz refuses to start the
blend and the status bar tells you why.

## Opening the Blend popover

Blend is configured in a **popover anchored to the Blend toolbar
button**. Three ways to open it:

- **Path → Blend** menu item.
- **Ctrl + B** keyboard shortcut.
- **Right-click the Blend toolbar button.**

A **left-click on the Blend toolbar button** is a separate path:
it re-applies the *last* Blend settings to the currently selected
two paths without opening the popover. If the node counts don't
match, left-click falls through and opens the popover so you can
see the warning banner and hit Equalize.

The popover opens non-modally — the canvas stays interactive so
you can click somewhere else to bail out, or pick a different pair
without losing the popover state.

## ❶ Steps

The **Steps** spinner ❶ is the number of *intermediate* copies the
blend produces, from 1 to 50. Plus the two source paths, that's
`steps + 2` total objects in the result. Default is 4.

More steps = smoother transition, more rendering cost. For tapered
strokes 8–12 is usually plenty; for elaborate morphs you might
want 30+. Curvz caches the intermediate geometry, so once a blend
exists the cost is paid only when something changes.

## ❷ Reverse direction

The **Reverse** checkbox ❷ swaps which source is treated as A
(start) and which is B (end). The visible effect depends on the
geometry — for symmetric path pairs Reverse changes nothing, but
for paths with a clear start/end (a comma curve, an arc, an arrow
shape) it flips the blend's direction.

If the result is going the wrong way, toggle Reverse rather than
deleting and re-picking the paths in the opposite order.

## ❸ Stroke width range

The **Override stroke width range** checkbox ❸ exposes two
spinners — **Start** and **End** — for the stroke widths the blend
should interpolate between. By default, the blend interpolates the
two source paths' stroke widths (A.stroke.width → B.stroke.width)
the same way it interpolates colour and geometry, and the spinners
sit dimmed.

Toggling the override on lets you set a *different* width range
that overrides the source widths. Useful for the tapered-stroke
pattern: build two identical-geometry paths with the same
stroke width, then enable the override and set, say, 4 and 0.25
for a stroke that pinches off into nothing.

## ❹ Node-count mismatch warning

When the two source paths have different node counts, the popover
shows a warning banner ❹ at the top with an **Equalize** button.

Click **Equalize** and Curvz inserts nodes into whichever path
has fewer nodes until both sides match. The insertion is
geometry-preserving — Curvz finds the longest segment by arc
length and splits it at its midpoint using De Casteljau
subdivision (which keeps the curve shape exactly identical),
repeating until counts match.

The mutated path has its own undo entry. Once counts agree the
banner goes away and you can hit **Blend**.

## What gets produced

Hitting **Blend** replaces the two source paths in the document
with a **Blend container** at the lower-z position of the
original two. The container holds the cached intermediate steps
plus references to the two sources. In the Layers panel it
appears as a single Blend row that you can collapse to inspect
its contents.

The intermediate steps are not directly editable: they're cached
output from the morph. Edit the **two source paths** to change
the result. Curvz tracks the sources and rebuilds the cache on
the next draw whenever they change.

If editing didn't trigger a rebuild for some reason — a property
that wasn't covered by the dirty signal, an external change —
right-click the Blend and choose **Rebuild Blend Steps**. The
verb is in the **Effects** submenu of the canvas right-click
context menu and on the Blend row's right-click menu in the
Layers panel; either surface works.

## Releasing a blend

Select a Blend and choose **Path → Release Blend** (or press
`Ctrl + Shift + B`). The two source paths return as normal
siblings, the cached steps return as a group named "Steps"
between them, and the Blend container is removed. The result
preserves the bake: you keep the geometry the blend produced as
ordinary editable paths.

If you want only the sources back without the bake, delete the
Steps group after releasing.

## What blends interpolate

Across the morph, each property on the intermediate steps is
linearly interpolated:

- **Anchor positions** and **handle positions** (so the path
  shape morphs).
- **Fill colour** and **stroke colour**.
- **Stroke width**, unless the override is enabled (which uses
  the popover's range instead).
- **Opacity**.

Properties that don't interpolate continuously (dash arrays of
different lengths, blend modes, swatch references) take the value
from the start source for every intermediate.

## Where to next

- For *cloned* sequences without geometric morphing, see
  **Step and Repeat** (7.1).
- For per-path stroke and fill controls, see **Styling**
  (5.4.5).
- For combining paths into one shape rather than animating
  between them, see **Boolean operations** (8.2).
- For trimming output to a frame, see **Clip masks** (7.2).
