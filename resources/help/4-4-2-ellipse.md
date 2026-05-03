# ![Ellipse tool icon](img/icons/curvz-oval-symbolic.svg) Ellipse

The **Ellipse tool** drops an axis-aligned ellipse on the canvas.
Like the Rectangle, it's a drag-to-create parametric tool with the
same modifier set, plus a right-click popover for exact dimensions.

Activate it from the toolbox or with the **E** key (canvas focus
required).

![Ellipse tool mid-drag](img/4-4-2-ellipse-drag.png)

## Drag to draw

The gesture mirrors Rectangle exactly:

1. Press the mouse on the canvas at one corner of the ellipse's
   bounding box.
2. Drag toward the opposite corner. The ellipse previews live,
   inscribed in the bounding rectangle.
3. Release to commit.

After commit the new ellipse becomes the selection and Curvz
returns you to the **Selection tool**.

The ellipse is created as a path with four nodes — one at the top,
right, bottom, and left of the ellipse — and Bézier handles tuned
to approximate the perfect curve. Like the rectangle, it's a
regular path once drawn; there's no "ellipse primitive" preserved
behind the scenes.

## Modifiers while dragging

The same modifier set as Rectangle:

- **Shift** — constrains to a **circle** (equal width and height).
- **Alt** — draws **from the centre** out. The press point is the
  ellipse's centre; the drag defines the radius vector to the
  bounding-box corner.
- **Shift + Alt** — a centred circle. Especially useful for icon
  work where a circle anchored on the artboard centre is a common
  starting shape.

Modifiers are evaluated at release, so press or release them
mid-drag freely.

## Right-click for exact dimensions

Right-click the Ellipse button in the toolbox to open the **Place
Ellipse** popover. The fields match Rectangle's:

- **X**, **Y** — the bottom-left corner of the ellipse's bounding
  box in document coordinates. (Curvz uses bounding-box corner,
  not centre, for consistency with Rectangle.)
- **W**, **H** — the bounding-box width and height.
- **Units** — read-only label showing the active display unit.
- **Place** button — commits and dismisses.

For an ellipse defined by *centre + radii*, compute it as `X =
centre_x − radius_x`, `Y = centre_y − radius_y`, `W = 2 ×
radius_x`, `H = 2 × radius_y`. The Curvz inspector keeps that
mental model — bbox-relative — throughout the app, so the popover
matches.

## Pixel-grid behaviour

In Pixel mode (5.3.2), the ellipse's bounding-box corners land on
integer pixel positions. The curve itself is rendered with
sub-pixel precision; only the bounding extents are snapped.

For very small ellipses (under about 8 pixels per axis) the
four-node Bézier approximation can read as slightly faceted at
high zoom. For sub-icon-sized circles, consider using the
**Polygon tool** (4.4.4) with many sides instead.

## What's on the ellipse after creation

The new ellipse inherits the active fill and stroke from the
toolbar's fill/stroke well, exactly like the Rectangle tool.

Editable as a regular path: the Node tool (4.2.2) lets you drag
any of the four anchors or their handles, which deforms the
ellipse into an organic curve. To split the ellipse into a half-
or quarter-arc, place a node on the curve with the Node tool, then
press **B** to break.

## Where to next

For polygons and stars (parametric shapes with adjustable side
counts), see **Polygon** (4.4.4). For freeform curves, the **Pen**
(4.3) and the **Node tool** (4.2.2) cover the rest of the
shape-creation territory.

The **Rectangle** tool (4.4.1) and the Ellipse tool share their
gesture and modifier conventions — once you know one, you know
both.
