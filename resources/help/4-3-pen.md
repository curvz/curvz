# ![Pen tool icon](img/icons/curvz-pen-symbolic.svg) Pen

The **Pen tool** is for drawing freeform Bézier paths node by node.
You click to drop anchors and drag at each click to extend the
handles. The result is a path with exactly the geometry you placed,
no parametric constraints — the inverse of the Shape tools, which
build paths for you from a single drag.

Activate the Pen from the toolbox or with the **P** key (canvas
focus required).

![A Pen-tool path mid-creation, with placed nodes and a live preview segment](img/4-3-pen-path.png)

## The basic flow

A Pen-tool session looks like this:

1. **Click** to drop the first anchor.
2. **Click** again somewhere else to drop the next anchor. Curvz
   draws the segment between them.
3. Repeat for as many anchors as the path needs.
4. To **close** the path, click the first anchor again — Curvz
   highlights it as you approach to confirm the close. To **finish
   the path open**, press **Enter**.

A new path commits as a real document object only when you close
or finish it. While the path is in progress (a "WIP" path) Curvz
shows it with a temporary preview style.

To **discard** the WIP path entirely, press **Esc**. Switching
tools also commits the open path automatically — no nodes are
lost on tool switch.

## Curved versus straight segments

The Pen has two click modes that determine the segment shape:

- **Click** (no drag) — drops a Corner anchor with retracted
  handles. The segment from the previous anchor is **straight**.
  Useful for polygonal outlines where every angle is hard.
- **Click and drag** — drops an anchor and pulls out its outbound
  handle in the direction of the drag. The segment from the
  previous anchor is **curved**, with the curve's tension matching
  how far you dragged. Release to commit. Useful for organic
  shapes.

You can mix the two freely — click for some anchors, drag for
others. The path adapts to whatever you produce.

## Modifier-while-clicking — node type

While dragging the handle out of a new anchor, hold a modifier to
control the node's type:

- **No modifier — Symmetric** — the inbound handle mirrors the
  outbound: same length, same angle through the anchor. The
  smoothest possible curve.
- **Shift — Cusp** — the inbound handle locks to the previous
  segment's tangent at one-third its length. The outbound handle
  is whatever you drag. The two handles can have different angles
  through the anchor — produces a soft kink.
- **Alt — Corner** — handles are independent in length and angle.
  Drag the outbound; the inbound resolves to whatever produces
  the cleanest segment from the previous node. The hardest break
  in the curve direction.

The modifier is read at the moment of click. A Cusp node with the
inbound mid-drag adjusted by Shift is committed as Cusp; releasing
Shift after the click does not change it.

## Continuing an existing path

When the Pen is active and there is **no WIP** path yet, clicking
within 8 pixels of an open path's head or tail node **resumes**
that path. Curvz removes the existing path from the document
temporarily, loads it as the new WIP, and continues from the
clicked endpoint. Subsequent clicks extend the path.

If you abandon the resume (Esc), Curvz restores the original path
unchanged. Click somewhere on the canvas first, then resume — the
resume gesture only fires when you have no WIP active.

To **add a node to a closed path** or to a path's interior, use
the Node tool (4.2.2) instead — click the segment to insert.

## Snapping to existing nodes

The Pen tool snaps to existing path nodes within 6 pixels. When
you click near an anchor on any path in the document, Curvz uses
that exact node position rather than the click location. Useful
for connecting new artwork to existing artwork at exact points.

The snap is screen-pixel based, so it scales with zoom — zoom in
for fine work, zoom out for forgiving snaps over larger
distances. The 6 px tolerance is below the close-detection 8 px
tolerance for the path's own first node, so the close gesture
takes priority when you are near the path you are drawing.

## Closing the path

Two ways to close:

- **Click the first anchor** of the WIP path. Curvz highlights it
  as you approach so you know the close gesture is armed. The
  click commits the path as closed and finishes the WIP.
- **Click anywhere else then press Enter** to finish open instead.

Open paths are visually identical to closed ones until you fill
them — fill rendering needs a closing segment to know which side
of the path is "inside." For stroke-only icon work, open and
closed paths look the same.

## What the Pen cannot do

A few related operations live in the Node tool (4.2.2) rather than
the Pen:

- **Editing handles** of existing anchors — switch to the Node
  tool, click the anchor, drag the handles.
- **Inserting an anchor on an existing segment** — switch to the
  Node tool, click on the segment.
- **Joining two open paths into one** — Node tool, select an
  endpoint on each, press **J**.
- **Reversing path direction** — Node tool, **R**.

The split is intentional: the Pen creates, the Node tool edits.

## Where to next

- **Node tool** (4.2.2) — refine what the Pen draws.
- **Selection tool** (4.2.1) — move whole paths once they are
  drawn.
- **Color picker & paint editor** (3.7) — set fill and stroke
  before drawing, or after.

If the artwork you need is parametric — a rectangle, ellipse,
polygon, line, spiral — those have dedicated tools in the **Shape
tools** family (4.4) that are usually faster than building the
shape node-by-node with the Pen.
