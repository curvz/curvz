# Blend

The **Blend** section controls a Blend container — an object Curvz
generates when you ask it to interpolate between two source paths
in a regular sequence of steps. The section appears in the
inspector for any selection where the primary node is a Blend.

> **Appears when:** the primary selection is a Blend container,
> created via **Object → Blend**. Selecting two paths and applying
> Blend wraps them in a Blend node along with a generated cache of
> intermediate steps; that container is what this section edits.
> Blend is an open-ended chapter (7.3) — this page covers only the
> inspector controls.

![The Blend section showing STEPS, A/B node counts, reverse, stroke override, and Release](img/5-4-3-blend-section.png)

## ❶ STEPS

A spinner from 1 to 50, controlling how many intermediate paths
Curvz generates between source A and source B. Step 1 produces a
single midway path; higher counts produce a denser sweep. Defaults
to whatever the Blend was created with.

The step count is a quality-versus-cost trade-off:

- **Low** (3–5) — quick to render, lighter document. Good for
  exploration and low-step gradient effects.
- **Medium** (8–15) — most icon work. Reads as a smooth blend
  without overwhelming the SVG.
- **High** (20+) — heavy. Use when the result is visually
  continuous (a pseudo-gradient) rather than a countable sequence.

Editing the spinner triggers a cache rebuild on the next draw, so
the canvas updates within a frame.

## ❷ A/B NODES

A read-only label showing the node count of the two source paths
in the form `A / B  (locked)`. Both counts are locked once the
Blend is built — Curvz needs A and B to share a node count for the
interpolation maths to work. If you need to change a source's
shape, edit it before the blend is created, or release the blend,
edit, and re-blend.

## ❸ Reverse direction (swap A↔B)

A checkbox that swaps which source is A and which is B. Tick it
and the cache rebuilds with the steps running in the opposite
direction; visually the sweep flips end for end. The checkbox
auto-resets to off after firing — it's an action, not a persistent
state. The swap is undoable as a single step.

## ❹ Override stroke width range

By default, the stroke width on each generated step linearly
interpolates between A's stroke width and B's. Most of the time
that does what you want. The **override** checkbox lets you
specify a custom range without touching A or B's actual stroke
widths.

When **off** (the default), the **START W** and **END W** fields
below are dimmed and read A's and B's stroke widths. When **on**,
both fields become editable and the override values feed the cache
rebuild instead.

Use this when you want a thick-to-thin sweep without changing the
endpoint paths' own stroke widths — for example, when A and B both
need to render at a fixed width when shown alone but the blend
benefits from a stroke taper.

## ❺ START W and END W

Two spin buttons — stroke width at the A end and the B end of the
blend. Live until you flip the override checkbox; after that they
hold whatever you typed. Both respect the document's display unit.

## ❻ Release

The button at the bottom of the section dissolves the Blend
container into its constituent parts:

- **A** survives as its own object.
- **B** survives as its own object.
- The cached step paths between them are baked into a **Group**
  containing one path per step.
- All three become siblings in the parent layer.

Once released, the Blend's parametric link to A and B is gone —
edits to A no longer regenerate the steps. Use Release when you
want to manually edit individual steps, when the blend is the
final output you want as plain SVG, or when you need to apply
appearance changes that don't propagate cleanly through the cache.

Release is undoable as a single Ctrl+Z step.

## What's not in the Blend section

A few related controls live elsewhere:

- **Source A and B's appearance** is edited by selecting the
  source individually inside the Blend container (use the Layers
  panel to dive in). Their Appearance widgets work as for any
  path. The cache regenerates on the next change.
- **The Blend's own fill/stroke** is unusual — the cache picks up
  per-step interpolated paint from A and B. The container's own
  Appearance values are ignored. To recolour a blend, recolour A
  and B; the cache follows.

## Where to next

For the conceptual side — what blends are, when they're useful,
how to create one — see **Blends** (7.3) under Working with
objects. For step counts and source-shape requirements, the
chapter has worked examples.

If the blend you're looking at is on a path you want to release
and edit further, **Release** is the action you want; the
remaining workflow is just standard path editing.
