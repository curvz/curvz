# Margins

The **Margins** section configures the document's margin and
column-grid overlay — the inset frame and column/row breakdown
you see on a printed page or a layout-driven design. Like
guides and grid, margins are document-scoped, never export with
the SVG, and exist only as a layout helper while drawing.

Margins are **off by default**. Enable from this section, then
set inset distances and (optionally) a column/row breakdown.

![The Margins section with TOP/BOTTOM, LEFT/RIGHT, and column/row pairs visible](img/5-3-5-margins-section.png)

## ❶ Enable

A single checkbox at the top toggles the margin overlay on and
off. Disabled, the section collapses to just the toggle. Enabled,
the section expands and the canvas starts drawing the margin
frame.

Curvz remembers your last margin geometry within a session, so
you can flip the overlay on and off without retyping numbers.

## ❷ MARGINS — TOP / BOTTOM

A pair of numeric fields under a **MARGINS** header:

- **TOP** — distance from the top edge of the artboard to the
  top of the printable area.
- **BOTTOM** — distance from the bottom edge of the artboard to
  the bottom of the printable area.

Both spinners use the document's display unit. Standard print
margins are typically 12–25 mm; icon work usually wants smaller
values (or zero) to keep the artboard usable to its edges.

## ❸ LEFT / RIGHT

A second pair, on the row below:

- **LEFT** — distance from the left edge of the artboard to the
  left of the printable area.
- **RIGHT** — distance from the right edge to the right of the
  printable area.

The four insets are independent — set them asymmetrically when
you need to (e.g. binding margins for a left-bound book, where
the inside edge is wider than the outside).

## ❹ COLUMNS — COUNT / GAP

A pair of fields under a **COLUMNS GAP** header:

- **COUNT** — how many vertical columns to draw inside the
  margin frame. Whole numbers from 1 to 100. **1** disables
  column subdivision while keeping the margin frame visible.
- **GAP** — the gutter between columns, measured in the
  document's display unit.

Curvz computes column positions automatically: it takes the
horizontal printable width (artboard width minus left and
right insets), subtracts (count − 1) gutters, and divides
the remainder evenly into `count` columns.

This is the same model as a print or web grid system. Twelve
columns is a Web convention; six or eight is more common in
print layouts. For icons, leave it at 1 — column subdivision
rarely makes sense at icon sizes.

## ❺ ROWS — COUNT / GAP

A second pair under a **ROWS GAP** header. Identical shape to
the column row, but for horizontal subdivisions:

- **COUNT** — number of horizontal rows.
- **GAP** — gutter between rows.

Most documents leave this at **COUNT = 1** (no row subdivision).
Set it higher when working on a multi-band layout — a banner
with stacked panels, a calendar grid, or a mosaic of tiles.

## ❻ Color

A wide colour swatch shows the margin overlay's colour. Click to
open the colour picker, which here also exposes alpha — like
the grid, margins are drawn translucently so they do not compete
with artwork. A muted secondary or accent colour at 30–50% alpha
reads cleanly without dominating.

The colour applies to the entire overlay — the inset frame,
the column gutters, and the row gutters all draw in this one
colour. There is no per-axis colour control.

## Snap to margins

Like the other layout overlays, margins do nothing on their own
unless you opt into snapping. The toggle is in the **Snap**
section (5.3.6) as **Snap to margins**. With snap to margins
enabled, dragging or nudging catches on the inset frame edges
and on every column/row gutter line.

## Locking and disabling

Margins live on their own document layer (`Margins`) which can
be locked from the **Layers** panel. A locked margin layer
greys out every control in this section, including spinners.

Unticking **Enable** at the top removes the margin layer from
the document. To hide the margin overlay temporarily without
losing geometry, toggle the layer's visibility in the Layers
panel instead.

## Where to next

The full layout overlay trio is **Guides** (5.3.3), **Grid**
(5.3.4), and **Margins** (5.3.5). All three feed into **Snap**
(5.3.6), which decides which of them the cursor catches on.

If you are coming to Curvz from a print-layout background,
margins here are deliberately kept simple — Curvz is an icon
editor, not a layout app, so binding-side and bleed
distinctions are not surfaced. Use the four-inset model for
the page frame and the column/row counts for any subdivision
you need.
