# Clip masks

A **clip mask** is a path used to crop the visible region of other
artwork. Anything inside the clip path is shown; anything outside
is hidden. The objects underneath aren't destroyed — they're still
there in full, just rendered through a window cut to the clip
shape. Release the clip later and the original objects come back
intact.

Clipping is how you keep complex artwork inside a frame without
having to chop the artwork to fit. Common uses: trimming detail to
the artboard edge, framing illustrations inside a circle, masking
photo-style icons to a rounded rectangle.

![A path masked by a circular clip](img/7-2-clipmask-result.png)

## Setting up a clip

Two-step gesture, both steps with the canvas focused:

1. **Select the objects** you want clipped. Multi-select is fine —
   select the whole subject of the clip.
2. Trigger **Clip** from the **Path** menu (or press `Ctrl + 7`).

Triggering Clip puts Curvz into **clip-pick mode**. The status bar
shows that you're armed; the next click is interpreted as
"that's the clip shape." Click any Path or Compound on the canvas
and Curvz wraps the selection plus the clicked path into a single
ClipGroup container.

If you click somewhere that isn't a Path or Compound (or click
empty canvas), pick mode cancels and nothing happens. Press `Esc`
to abort the pick before clicking anything.

## What ends up where

After a successful clip:

- A **ClipGroup** node replaces the original selection in its
  parent's children. It sits where the lower-z member of your
  selection was (so the clipped result lands where the artwork
  was, not where the clip shape was).
- The **clipped objects** become children of the ClipGroup.
- The **clip path** is stored separately on the ClipGroup as its
  *clip shape*. It is no longer rendered as ordinary geometry —
  it acts only as the cropping outline.

The ClipGroup behaves as a single object for selection, drag,
move, and z-order. It appears in the Layers panel as one row,
with the clipped contents nested inside; the **clip shape**
shows as the topmost pseudo-row in the expanded group, marked
with a `✂` badge.

## Editing inside a clip

Open the **Layers** panel (6.2) and expand the ClipGroup row
(click its `▸` arrow). You'll see:

- The **clip shape** as the topmost pseudo-row, badged `✂`.
- The **clipped objects** below it, in z-order, nested under the
  group.

Click any of those rows to make that node the canvas selection,
then edit it with whatever tool fits — the Node tool to push
nodes around, the Selection tool to move/scale, the Text tool to
re-edit a text child, and so on. The rendered output remains
masked while you work; only the artwork being edited shows its
normal handles.

Editing the **clip shape** uses the same gesture: expand the
ClipGroup row, click the `✂` pseudo-row, switch to the Node tool.
The mask reshapes in real time as you drag its nodes — no need to
release the clip to adjust the cropping outline.

To return to selecting the whole ClipGroup as one object, click
the ClipGroup itself in the Layers panel, or click on any of its
visible artwork on the canvas (canvas hits always promote to the
top-level container).

## Releasing a clip

To dissolve a ClipGroup, select it and choose **Path → Release
Clip** (or press `Ctrl + Alt + 7`). The contents and the clip
shape are returned to the ClipGroup's parent as ordinary
siblings, and the ClipGroup itself is removed. The selection
becomes the released children.

The released clip path comes back as a normal path in its
original geometry — same fill, stroke, and node data it had when
you first built the clip. Useful when you need to adjust the mask
shape: release, edit, re-clip.

## Deleting through a clip

Deleting one of the clipped objects from inside a ClipGroup
removes just that object — the group survives. Deleting the
**clip shape** collapses the group: Curvz unwinds the wrapper as
if you'd pressed Release before deleting, with the clipped
artwork returned to the parent as ordinary siblings. This keeps
clipped content from being orphaned when the mask itself is
deleted.

## Clipping with compound paths

The clip shape can be a single path or a Compound path. Compound
paths give you holes — the standard "donut" pattern (outer outline
plus inner outline marked as a hole) works as a mask the same way
it works as a fill, with even/odd rule producing the cut-out.
Build the compound first (Ctrl+8 from a multi-path selection),
then use it as the clip in the two-step gesture above.

## What clips don't do

Two practical limits worth knowing:

- **A clip mask doesn't paint the clipped contents.** It controls
  visibility only. The clipped objects keep their own fill,
  stroke, and other appearance properties.
- **Nested clips work but stack their masks.** A ClipGroup inside
  a ClipGroup will be visible only where *both* masks are visible
  — same semantic as SVG.

If you want to *fill* a clip's outline with a colour as well as
crop to it, group a coloured copy of the clip path with the
artwork and clip both together.

## Where to next

- The **Layers** panel (6.2) shows ClipGroups as a single row
  with their children nested inside.
- For combining paths into a single masking outline (the donut
  case), see **Compound & derived** (8.3).
- For boolean cuts that *remove geometry* rather than just hide
  it, see **Boolean operations** (8.2). Subtract gives you a
  permanent shape change rather than a mask.
- For interpolative effects between two paths, see **Blends**
  (7.3).
