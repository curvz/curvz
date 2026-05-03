# Align & Distribute

**Align** moves selected objects so their edges or centres line up.
**Distribute** spaces objects out evenly along an axis. Both work on
the current selection; both need at least two objects selected.

Reach the eight ops three ways:

- **Toolbar** — the Align & Distribute button below the tools opens
  a popover with all eight ops as icons. The button is disabled
  unless the gate below is satisfied.
- **Align menu** — every op has a menu entry. The six align ops
  also have keyboard shortcuts (see **Keys** at the end of this
  topic). The two distribute ops are menu-only.
- **Macro recording** — every align and distribute op is a
  recordable MacroStep. See **Macros** (11.1).

![The Align & Distribute popover with align row above and distribute row below](img/7-4-align-popover.png)

## When the ops are available

Align & Distribute is **only active when both** of these are true:

- The **Selection tool** is the active tool.
- The selection contains **two or more objects**.

If either condition fails, the toolbar button is greyed out and the
menu items are disabled. The two-object minimum is structural —
align "to what?" needs at least one other object as the reference,
and distribute needs at least three to do meaningful spacing
(two-object distribute is a no-op since two endpoints can't be
spread further than themselves).

## The six align ops

Each align op picks an edge or centre of the selection bounding
box and moves every selected object so its corresponding edge or
centre lines up with that line:

- **Align Left** — every object's left edge moves to the
  leftmost edge in the selection.
- **Align Center H** — every object centres horizontally on the
  selection's vertical centre line.
- **Align Right** — every object's right edge moves to the
  rightmost edge in the selection.
- **Align Top** — every object's top edge moves to the topmost
  edge in the selection.
- **Align Center V** — every object centres vertically on the
  selection's horizontal centre line.
- **Align Bottom** — every object's bottom edge moves to the
  bottommost edge in the selection.

The bounding box used is the union of every selected object's
bounding box — unless an anchor or reference point is in play
(see below).

## The two distribute ops

- **Distribute Horizontally** — keeps the leftmost and rightmost
  objects in place; moves the in-between objects so the
  horizontal gaps between consecutive objects are equal.
- **Distribute Vertically** — same idea, vertical axis.

Distribute uses object **bounding boxes** to compute gaps, not
centres. Two objects of different widths placed by Distribute
Horizontally will have the same gap *between* them as every
other pair, even though their centres are not evenly spaced.
This matches Affinity Designer and Illustrator behaviour and is
usually what icon work needs (visual rhythm comes from gaps,
not centres).

The anchor (next section) is **ignored** by distribute — there
is no meaningful "anchor" semantics for spacing.

## The anchor — pinning one object as the key

By default, an align op moves *every* selected object, including
the one you might think of as the "reference". If you select a
big shape and a small shape and Align Left, both move — they end
up with their left edges lined up at the leftmost edge of the
combined selection.

Sometimes you want one object to stay put and the others to move
to match it. That object is the **align anchor**.

To set an anchor, **Ctrl + Alt + click** the object you want to
pin. Curvz draws a small anchor glyph at its centre to confirm:

![A three-object selection with the anchor glyph showing on the chosen key object](img/7-4-align-anchor-glyph.png)

While the anchor is set, every align op uses the **anchor's
bounding box** as the reference instead of the selection union.
The anchor stays put; everything else aligns to it.

The anchor is **selection-scoped**:

- A second Ctrl + Alt + click on the same anchored object clears
  the anchor (the object stays selected).
- The anchor automatically clears the moment the marked object
  leaves the selection (deselect, delete, undo to before its
  selection — any of those drop the anchor).
- Distribute ignores the anchor entirely.

The anchor is meant for one quick pass: mark the key object,
align several others to it, move on. There is no save-with-document
state to manage.

## How the anchor interacts with reference points

Curvz also has **reference points** (4.7) — persistent placement
markers on the canvas that act as alignment targets independent
of selection. When both an anchor and reference points are
present, the anchor wins for align ops:

1. **Anchor** (Ctrl + Alt + click on a selected object) — used if
   present.
2. **Reference points** — used if no anchor is set and at least
   one reference point is selected as part of the operation.
3. **Selection union** — used if neither of the above applies.

This priority is intentional. Reference points are persistent —
you set them up early in a layout and use them many times.
Anchors are momentary — selection-time intent that should
override the persistent geometry for the length of one click.
After the click, the reference points are still there for the
next op.

Distribute uses the union of every movable object plus any
reference points; it does not honour the anchor.

## Undo

Each align or distribute op is one undo step. **Ctrl + Z**
reverts the entire op — every object snaps back to its
pre-op position in one go.

If you ran an align you didn't want, the cleanest fix is undo,
not "align it back" — alignment is not symmetric (Align Left
followed by Align Right does not return to the original
positions, since the leftmost and rightmost edges are different
references).

## Where to next

- The **Macro** system (11.1) records align and distribute ops
  as MacroSteps — useful when "align then distribute" is a
  combo you do constantly. Star a macro that does both and run
  it from the Layers panel macro strip.
- **Reference points** (4.7) are the persistent counterpart to
  the per-click anchor. If you find yourself anchoring the same
  object every session, a reference point near that geometry
  may be a better fit.
- The **Selection inspector POS row** (5.4.1) does single-object
  numeric placement — use it when "align to a number" is what
  you want, instead of "align to another object".

### Keys

These hotkeys only fire when the canvas has focus and the gate
above is satisfied (Selection tool + 2+ objects selected):

- `Ctrl + Alt + L` — Align Left
- `Ctrl + Alt + H` — Align Center Horizontal
- `Ctrl + Alt + R` — Align Right
- `Ctrl + Alt + P` — Align Top (mnemonic: to**P**)
- `Ctrl + Alt + M` — Align Center Vertical (Middle)
- `Ctrl + Alt + B` — Align Bottom

The two distribute ops are menu-only (Align → Distribute
Horizontally / Distribute Vertically). They are used less often
than alignment and reserving four-finger combos for them is not
worth the muscle-memory cost.

The anchor toggle is canvas-side, not a window hotkey:

- `Ctrl + Alt + click` on a selected object — toggle align anchor
