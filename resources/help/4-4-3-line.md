# ![Line tool icon](img/icons/curvz-line-symbolic.svg) Line

The **Line tool** draws straight-line paths — either single
segments or multi-vertex polylines. Unlike Rectangle and Ellipse,
which drag to create one shape at a time, the Line tool is
**click-to-place**: each click drops a vertex, and you can keep
clicking to extend the path.

Activate it from the toolbox or with the **L** key (canvas focus
required).

![Line tool mid-creation, with placed points and a live preview to the cursor](img/4-4-3-line-drag.png)

## Click to place

The basic flow:

1. Click on the canvas to drop the first vertex.
2. Move the cursor — Curvz draws a live preview line from the
   last vertex to the cursor.
3. Click again to drop the next vertex. The preview line
   commits as a real segment.
4. Continue clicking to extend the path. Each click adds another
   vertex.
5. Finish with one of:
    - **Click on the first vertex** (Curvz snaps when you're
      within 8 pixels) to **close** the polyline as a closed
      path.
    - **Double-click** anywhere to commit the path **open** at
      the previous vertex. (The double-click drops a duplicate,
      which Curvz removes for you.)
    - **Press Enter** to commit open at the current cursor.
    - **Press Esc** to discard the WIP entirely.

Like the Pen tool, the Line tool only commits a real document
object when you finish or close. Until then, the in-progress path
is a temporary preview.

After commit, Curvz returns you to the **Selection tool**, exactly
like Rectangle and Ellipse.

## Shift to constrain angle

Hold **Shift** while moving the cursor (or while clicking) to
constrain each new segment to a **15° increment** off the previous
vertex. The preview snaps live, so you can see the constraint
engaged.

The 15° increments cover the common cases — horizontal, vertical,
45° diagonals, and the in-between sixteenths-of-a-circle — without
being so coarse that you can't draw an arbitrary shape. For
freehand precision, release Shift and the cursor moves freely
again.

## Snap to start

When the cursor approaches the first vertex within 8 pixels (in
screen space, so it scales with zoom), Curvz **snaps the cursor
to the start** and arms the close gesture. A click in this state
finishes the path as closed. The visual feedback is the cursor
position locking to the start point — once you see that, you know
the next click closes.

If you don't want to close — say, you happened to drag near the
start mid-path — move the cursor away to disarm and continue
extending the path normally.

## Right-click for exact dimensions

Right-click the Line button in the toolbox to open the **Place
Line** popover. The popover places a single straight segment
between two points:

- **X1**, **Y1** — start point in document coordinates.
- **X2**, **Y2** — end point.
- **Units** — read-only label showing the active display unit.
- **Place** button — commits and dismisses.

The popover bypasses the click flow for cases where you know both
endpoints exactly. It only places **one** segment per Place
click — for a multi-vertex polyline use the click-to-place flow
instead.

## Single-segment versus polyline

Two clicks plus Enter (or a double-click on the second click)
gives you a single straight segment, which is useful for ruler
lines, alignment marks, or simple connections.

Continuing with more clicks builds a polyline — every segment is
straight, but the overall path can have as many vertices as you
need. Polylines export as a single SVG path with `L` commands
between every node.

For polylines that need curved segments mixed in, the **Pen tool**
(4.3) is the right choice — Line is intentionally straight-only.

## What the Line tool produces

The committed path:

- Has one node per click (no extra nodes for the close, since
  Curvz welds the start node when you click-close).
- Has all nodes as **Corner** type with retracted handles.
- Is **open** unless you closed it explicitly via the start-snap
  click.
- Inherits fill and stroke from the toolbar's fill/stroke well.

If you want to **convert straight segments to curves later**,
switch to the Node tool (4.2.2) and drag the handles out — the
straight segments become curves with whatever tension you set.

## Where to next

- **Pen** (4.3) — for paths that mix straight and curved segments.
- **Rectangle** (4.4.1), **Ellipse** (4.4.2) — drag-to-create
  closed shapes.
- **Ruler** (4.6.3) — when the line you actually want is a
  measurement, not a drawing.

If your path turns out to need curves between the existing
straight segments, you don't have to redraw — the Node tool can
add curvature to the existing path's segments after the fact.
