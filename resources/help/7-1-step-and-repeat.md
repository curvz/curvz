# Step and Repeat

**Step and Repeat** clones the current selection in a regular
sequence — N copies offset by the same X/Y from one to the next,
optionally rotating each copy around a pivot. It's the tool for
building radial spokes, arrow strips, evenly-spaced grids, and
anywhere you'd otherwise duplicate-and-nudge in a loop.

The operation is configured in a **popover anchored to the SnR
toolbar button**, with a live mini preview. Three ways to open it:

- **Path → Step and Repeat…** menu item.
- **Ctrl + Alt + D** keyboard shortcut.
- **Right-click the SnR toolbar button.**

A **left-click on the SnR toolbar button** is a separate path:
it re-applies the *last* Step and Repeat settings to the current
selection without opening the popover. Useful for repeating the
same operation across many sources once you've dialled in the
values you want.

You need at least one object selected; without a selection the
popover refuses to apply.

![The Step and Repeat popover with copies, offset, rotate and pivot fields](img/7-1-stepandrepeat-dialog.png)

## ❶ Copies

The **Copies** field ❶ is the number of *additional* objects the
operation will produce. The original stays in place; copies stack
on top of it in z-order in the same layer. Set it to 5 and you'll
end up with the original plus five clones — six items in total.

The operation is one undo step regardless of count. Ctrl+Z removes
all copies at once and restores the original to its solo state.

## ❷ Offset X / Y

The **Offset** spinners ❷ control the displacement applied between
each successive copy:

- **X** — horizontal offset in document units, positive to the
  right.
- **Y** — vertical offset in document units, positive *upward*
  (Curvz uses Y-up everywhere the user sees a number).

A copy is placed at `original + N × offset`, so an offset of
(20, 0) with three copies puts the original at 0, the first clone
at 20, the second at 40, the third at 60.

If you leave rotate disabled, this is the entire operation —
straight translate-and-clone, the way classic Step and Repeat
worked before Curvz added rotation.

## ❸ Rotate around a pivot

The **Rotate** checkbox ❸ enables per-copy rotation around a fixed
pivot point. When enabled it reveals two more controls:

- An **Auto / Fixed** mode dropdown.
- An **Angle** spin button.

In **Auto** mode, the angle is derived: `360° ÷ copies`. Eight
copies produces 45° each, twelve produces 30°. The result wraps
neatly back to the start, which is what you want for radial
patterns like clock faces, gear teeth, or compass roses.

In **Fixed** mode you type the per-copy angle directly. Useful
when you want a partial sweep (a fan of 5 copies at 15° each =
60° total) rather than a full revolution.

## ❹ Pivot

The pivot ❹ is the point that rotation orbits around. Two pairs of
spinners give you the X and Y in document units, with the same
Y-up convention as the offset.

The default pivot is the **selection's bounding-box centre**, set
the moment the popover opens. Override it via the spinners or
by dragging the pivot directly:

- **In the mini preview** — the dot in the small preview canvas is
  draggable. The spinners follow.
- **On the canvas** — while the popover is open Curvz draws a
  crosshair on the canvas at the pivot location. Click and drag
  the crosshair and the preview + spinners follow.

The two surfaces stay in sync; pick whichever one is easier to
target. Closing the popover (apply or cancel) hides the canvas
crosshair.

## ❺ Mini preview

The preview pane ❺ shows what the result will look like — a faint
outline of the selection's bounding rectangle plus stamps for each
copy. It updates live as you change Copies, Offset, Rotate, or
the pivot. It scales the bounding rectangle to fit, so you can see
the geometry of the result without worrying about absolute sizes.

The preview is approximate (it draws bounding rectangles, not the
real artwork). It's a sanity check on the *pattern*, not a render
of the final output.

## What gets cloned

Step and Repeat duplicates the selection at the SVG level — every
property of every selected object is copied: fill, stroke, dash,
stroke width, opacity, blend mode, and any binding to a swatch or
style. Bound copies stay bound: editing the swatch or style after
the operation re-paints every clone the same way it would a
single object.

If you select multiple objects, the *whole selection* is treated
as one unit — the bounding box used for default pivot covers all
of them, and each copy is the entire group of selected items
offset together.

The clones inherit the active layer. They are not grouped
automatically; if you want them as a single editable unit
afterwards, select them all and `Ctrl + G`.

## Sticky values

The popover remembers the last values you used between invocations
within a session — copies, offset, rotate state, angle, mode.
These are also what the toolbar SnR left-click repeats. The pivot
resets to the new selection's bounding centre on each open because
the previous pivot is rarely meaningful for a different target.
Restart Curvz and the values reset to defaults.

## Where to next

- For arranging existing objects without cloning, see
  **Align & Distribute** (7.4).
- For deriving a *new* shape from existing geometry (offset,
  expand stroke), see **Compound & derived** (8.3).
- For combining two paths interpolatively rather than by copies,
  see **Blends** (7.3).
- For fitting one icon's text along another's path, see
  **Text on Path** (4.5.2).
