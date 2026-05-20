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
- **Shift — Cusp** — the **inbound** handle retracts to the
  anchor (zero-length), and the **outbound** handle locks its
  direction to the **incoming tangent** of the previous segment.
  You still control its *length* by how far you drag. Produces a
  smooth curve into the new anchor that immediately changes
  direction on the way out — a soft kink.
- **Alt — Corner** — the inbound handle retracts to the anchor,
  and the outbound is fully free in both length and angle. The
  hardest break in the curve direction.

Both Cusp and Alt retract the inbound; the difference is whether
the outbound is constrained to the incoming tangent (Cusp) or
free (Corner). The modifier is read at the moment of click. A
Cusp node committed with Shift held stays Cusp even if you
release Shift afterwards — the type is baked at commit.

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

## Snapping when starting a new path

When the Pen is active and there is **no WIP** yet, the first
click snaps to two things:

- **Existing path nodes within 6 screen pixels** — Curvz uses
  the exact node position from any path in the document. Useful
  for starting a new path off an existing anchor.
- **Integer document units** otherwise — every click position
  rounds to whole doc-space units, so paths land on the pixel
  grid by default. This applies to subsequent clicks in the WIP
  too, not just the first.

The 6 px node-snap only fires on the first click of a new path
(i.e. when there is no WIP). Once you have a WIP active, the
close-detection 8 px tolerance on the WIP's own first node takes
over for the close gesture, and node-snap to *other* paths is no
longer in play — that's the design, to keep the rubber-band from
locking onto the path you're drawing.

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
