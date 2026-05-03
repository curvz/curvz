# Tools overview

The **toolbox** runs down the left edge of the window and holds
fifteen tools — every drawing, editing, and navigation surface you
need to draw an icon. This page is the one-paragraph rundown of
each tool, grouped the same way they appear in the toolbox, with
pointers into the deep-dive topics.

If you want to know **how the toolbox itself looks and behaves**
(active state, hover tooltips, the fill/stroke well at the bottom)
see **Toolbar** (3.3). This page is about what the tools *do*.

![The Curvz toolbox with each tool group called out](img/4-1-toolbar-tools.png)

## Selection & Node

Two tools at the top of the toolbox, separated from the drawing
tools below. These are the two you will reach for most often —
together they cover "pick something" and "edit it."

- **Selection (S)** — pick whole objects, move them, drag the
  bounding-box handles to scale or rotate, and use the Selection
  inspector section for precise transforms. Multi-select with
  Shift+click or marquee. See **Selection tool** (4.2.1).
- **Node (N)** — pick individual anchors and handles on a path,
  drag to recurve, alt-click to retract a handle, double-click a
  segment to insert a node. The companion to the Pen for editing
  whatever you have already drawn. See **Node tool** (4.2.2).

## Pen

A single tool for freeform path creation:

- **Pen (P)** — click to drop nodes one at a time, dragging at
  each click to extend the handles for a curved segment. Works
  exactly like every other Bézier pen tool you've used. Click the
  first node again to close the path, or press Escape to leave it
  open. See **Pen** (4.3).

## Shape tools

Five parametric shape tools — drag-to-draw on the canvas, or
right-click each tool's button to place at exact dimensions via a
dialog.

- **Rectangle (R)** — drag a rectangle. Hold Shift to constrain
  to a square. See **Rectangle** (4.4.1).
- **Ellipse (E)** — drag an ellipse. Hold Shift for a circle.
  See **Ellipse** (4.4.2).
- **Line (L)** — drag a straight line between two points. See
  **Line** (4.4.3).
- **Polygon (G)** — drop a regular polygon. Right-click the tool
  button to set sides. See **Polygon** (4.4.4).
- **Spiral (W)** — drop a parametric spiral. Right-click to set
  windings. See **Spiral** (4.4.5).

## Text family

Two tools that share a single text-node concept:

- **Text (T)** — click to place a text cursor, type the run.
  Switch tools to commit. See **Text** (4.5.1).
- **Text on Path (U)** — attach an existing text run to an
  existing path so the type follows the curve. See **Text on
  Path** (4.5.2).

## Utility tools

Three tools that don't draw new artwork but support the workflow
around the artwork you have:

- **Eyedropper (I)** — sample a colour from anywhere on the
  canvas and apply it to the current selection. Alt while
  clicking applies to stroke instead of fill. One-shot — Curvz
  restores the previous tool after a sample. See **Eyedropper**
  (4.6.1).
- **Zoom (Z)** — drag a marquee to zoom into a region.
  Right-click the tool button to set a precise zoom percentage.
  See **Zoom** (4.6.2).
- **Ruler (M)** — measure distance and angle between two points.
  Two clicks complete a measurement; configuration of how
  measurements behave lives in the **Measure** inspector section
  (5.3.7). See **Ruler** (4.6.3).

## Reference points and Corner

Two tools that don't fit cleanly into the other groups:

- **Reference Point (F)** — drop a non-printing reference point.
  Refs survive in the document but are stripped from exports.
  Useful for alignment hubs, registration marks, and design
  notes. Right-click the tool button for precise placement. See
  **Reference points** (4.7).
- **Corner (K)** — apply round, chamfer, or inverse-corner
  treatments to selected nodes on a path. A node-modifier rather
  than a shape-creator. See **Corner** (4.8).

## Tool switching at a glance

Every tool has a single-letter shortcut shown above. The keys only
fire when the canvas has focus — if a spin button or text entry
has focus, the same letter types into that field. Click the canvas
once or press **Esc** to return focus first.

When nothing is selected, **↑** / **↓** cycle through the toolbox
(previous / next tool). With a selection, the arrows nudge the
selection instead.

The Selection tool is the implicit default. After most one-shot
operations — Eyedropper sample, Zoom click, Ruler complete —
Curvz returns you to whichever tool you were on before. It's
designed so quick interruptions don't leave you stranded in the
wrong tool.

## Right-click on a tool button

Several tools — Rectangle, Ellipse, Line, Polygon, Spiral,
Reference Point, Zoom — open a parameter dialog when you
right-click their toolbox button. Use it for exact dimensions,
side counts, windings, or zoom percentages without dragging on
the canvas.

The right-click affordance is shown in the tool's hover tooltip.
Tools without a right-click dialog do nothing on right-click.

## The fill/stroke well

Below the tool list, near the bottom of the toolbox, are two
colour squares — the **fill well** and the **stroke well**. These
show the active paint pair and are clickable: click either to
edit the corresponding paint with the popover editor.

The wells are a quick-access mirror of the **Appearance**
inspector section (5.4.5). Picking from the well applies to the
current selection if there is one; otherwise it sets the default
paint for the next object you draw.

## Where to next

Each tool has its own deep-dive page (4.2.1 through 4.8). For the
toolbox container itself — its visual layout, the active-tool
highlight, the orientation of the fill/stroke well — see
**Toolbar** (3.3).

If you are starting from scratch and want a guided first walk
through the tools, the order roughly matches the page order: pick
a shape tool to draw something, switch to the Selection tool to
move it, switch to Node to refine its anchors, then to Pen for
custom shapes that don't fit a parametric.
