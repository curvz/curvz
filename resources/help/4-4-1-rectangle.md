# ![Rectangle tool icon](img/icons/curvz-rect-symbolic.svg) Rectangle

The **Rectangle tool** drops an axis-aligned rectangle on the
canvas. It's a parametric tool — drag to specify width and height,
or right-click the toolbox button for a dialog that places at
exact dimensions.

Activate it from the toolbox or with the **R** key (canvas focus
required).

![Rectangle tool mid-drag, with the live preview rubber-band](img/4-4-1-rect-drag.png)

## Drag to draw

The straightforward gesture:

1. Press the mouse on the canvas at one corner of the rectangle.
2. Drag toward the opposite corner. The rectangle previews live as
   you move.
3. Release to commit.

After commit, the new rectangle becomes the selection and Curvz
returns you to the **Selection tool** so you can move or transform
it immediately. This auto-switch is consistent across all
parametric shape tools.

The rectangle is created as a regular path with four nodes,
exactly the same kind of object the Pen tool produces. There is no
"rectangle primitive" that remembers it is a rectangle — once
drawn, it's just a path you can edit node-by-node.

## Modifiers while dragging

Hold during the drag to constrain the result:

- **Shift** — constrains to a **square**. Whichever of width or
  height has the larger drag distance becomes the side length;
  the smaller axis matches.
- **Alt** — draws **from the centre** out. The press point becomes
  the centre; the drag defines the half-diagonal. Useful when you
  know where the rectangle's middle should sit but not its corner.
- **Shift + Alt** — both at once: a centred square.

Modifiers are evaluated at release, so you can press or release
them mid-drag and the commit reflects whatever was held at the
end.

## Right-click for exact dimensions

Right-click the Rectangle button in the toolbox to open the
**Place Rectangle** popover:

- **X**, **Y** — the rectangle's bottom-left corner in document
  coordinates.
- **W**, **H** — width and height in document coordinates.
- **Units** — read-only label showing the active display unit
  (set in the **Canvas** inspector section, 5.3.2).
- **Place** button — commits the rectangle and dismisses the
  popover.

The popover defaults to a sensible starting size (about 20% of the
canvas, centred) when first opened. After placing once, it
remembers your last values for the next placement.

The popover bypasses the drag gesture entirely — use it when you
need a rectangle of an exact size, or when you want to drop one
without moving the cursor away from the toolbox.

## Drawing in pixel-perfect contexts

The Rectangle tool's commit position **rounds to whole units in
pixel mode**. For a canvas in Pixel mode (5.3.2), every rectangle
corner lands on an integer pixel grid by default. That matches the
expectation for icon work — pixel-grid alignment is the default
state, not something you have to opt into.

For physical-mode or ratio-mode canvases, the rectangle uses the
display unit's natural precision instead.

## What's on the rectangle after creation

The rectangle inherits whatever is on the **Toolbar's fill/stroke
well** at creation time. To preview a rectangle's appearance
before drawing, set fill and stroke first via the well (or via the
Appearance section, 5.4.5, with no selection).

After drawing, the rectangle behaves like any other path — switch
to the Node tool to round its corners with the **Corner** tool
(4.8), to break it into separate paths, or to drag individual
anchors. The Selection tool's bounding-box handles also do scale
and rotate from there.

## Where to next

The other shape tools — **Ellipse** (4.4.2), **Line** (4.4.3),
**Polygon** (4.4.4), **Spiral** (4.4.5) — share the same drag-to-
create pattern with their own modifier idioms. The same
right-click-to-configure affordance is on Ellipse, Line, Polygon,
and Spiral too.

For corner radii, draw the rectangle and then use the **Corner**
tool (4.8) to round, chamfer, or invert specific corners — Curvz
does not bake corner radius into the Rectangle tool itself.
