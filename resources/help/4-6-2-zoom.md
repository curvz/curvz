# ![Zoom tool icon](img/icons/curvz-zoom-symbolic.svg) Zoom

The **Zoom tool** changes the canvas's zoom level — the magnification
at which artwork is rendered. It does not change the document or
the artwork itself; it just changes what you see while editing.

Activate it from the toolbox or with the **Z** key (canvas focus
required). The cursor changes to a magnifying-glass shape over
the canvas.

![Zoom tool with a marquee drawn over a region of the canvas](img/4-6-2-zoom-marquee.png)

## What clicks do

Three click gestures:

- **Click** — zoom in 2×, centred on the click point.
- **Alt + click** — zoom out 0.5×, centred on the click point.
- **Drag a marquee** — zoom in to fit the marquee rectangle to
  the viewport. The marquee defines exactly the region you want
  to fill the editing surface.

Unlike the Eyedropper, the Zoom tool **stays active** after each
gesture — it's expected that you'll click several times in a row
while finding the right magnification. Press Z again or pick
another tool from the toolbox when you're done zooming.

A single short drag (less than 5 pixels) is treated as a click,
not a marquee, so accidental jitter doesn't produce a tiny
zoomed region.

## Marquee zoom-out

Holding **Alt** while dragging a marquee inverts the operation
— instead of zooming **in** to the marqueed region, it zooms
**out** so that what was previously the viewport now occupies
the marquee.

This is useful when you want to back off a precise amount: drag
out a smaller-than-current marquee, and the canvas zooms out by
that ratio.

## Ctrl+click — fit to window

**Ctrl + click** anywhere on the canvas with the Zoom tool active
fits the entire artboard plus a comfortable margin into the
viewport. Equivalent to **View → Fit Canvas** in the menu, but
faster when your hand is already on the Zoom tool.

## Right-click for exact zoom

Right-click the Zoom button in the toolbox to open the **Zoom
Level** popover:

- **Zoom** — a spin button accepting any value from **1% to
  9500%**. The canvas zooms to that exact percentage on Enter.
- **Preset buttons** — **25%, 50%, 100%, 200%, 400%** — five
  fixed zoom levels you'll reach for most often. Each preset
  click sets the zoom and dismisses the popover.

The 100% preset matches the document's natural pixel scale — at
that level, one document pixel takes one screen pixel.

The popover bypasses the click flow entirely, so it works even
when the canvas isn't focused. Use it when you need a specific
zoom percentage and don't want to drag-and-eyeball.

## Other ways to zoom

The Zoom tool isn't the only path to zoom — Curvz exposes the
operation throughout the workspace:

- **Ctrl + +** / **Ctrl + −** — zoom in / out by one step
  (canvas focus required).
- **Ctrl + 0** — fit canvas to window.
- **Ctrl + 1** — set zoom to 100% (1:1).
- **Ctrl + scroll wheel** — zoom toward the cursor by scroll
  amount. Most useful since the zoom centres on where you're
  looking, not on the canvas centre.
- **Status bar** — shows the current zoom percentage as a
  read-only label. (For interactive change use the Zoom tool's
  popover.)

The Zoom tool is the right choice when you want the **marquee**
gesture — for everyday "zoom in a bit" / "zoom out a bit" the
keyboard and Ctrl+scroll routes are usually faster.

## Pan during zoom

Zoom doesn't change which part of the canvas is visible, only
the magnification. To **pan** (slide the view) hold:

- **Middle-click drag** with any tool active.
- **Space + drag** with the left button, with any tool active.

Pan is independent of the active tool, so you can pan
mid-Zoom-tool to reposition before zooming further.

## Zoom limits

Zoom values are clamped to **1% to 9500%**. The lower bound
keeps icons larger than a single pixel; the upper bound keeps
the rendering pipeline from running out of headroom. In practice
neither limit comes up in normal icon work — useful zoom for
24-px icons sits in the 200%–800% range.

The marquee gesture clamps the result to the limits, so a marquee
that would zoom past 9500% just zooms to 9500% instead of
failing.

## Where to next

The companion tools are **Eyedropper** (4.6.1) and **Measure**
(4.6.3) — the three Utility tools that support the workflow
around the artwork.

For the **rulers** along the canvas edges (the visual coordinate
strips, not the Measure tool), see **Canvas, rulers & corner**
(3.6). They show the document's coordinate range at the current
zoom.
