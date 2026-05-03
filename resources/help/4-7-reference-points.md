# ![Reference points tool icon](img/icons/curvz-ref-symbolic.svg) Reference points

The **Reference Point tool** drops non-printing reference points on
the canvas — small markers that survive in the document but are
**stripped from exports**. Use them for alignment hubs,
registration marks, design notes, or any "this is the centre"
position you want to remember without baking it into the icon.

Activate it from the toolbox or with the **F** key (canvas focus
required).

![Two reference points placed on the canvas as small crosshair markers](img/4-7-refpoint-marker.png)

## What clicks do

The Ref tool's behaviour distinguishes between hitting an existing
ref and missing all existing refs:

- **Click on empty canvas** — drops a new reference point at the
  click position when you release the mouse button. The
  click-to-release workflow lets you nudge before committing —
  press, drag a tiny amount, release at the corrected position.
- **Click on an existing ref point** within 8 pixels — picks it
  up. Drag to reposition; release to commit the move. Click
  without dragging is a no-op (the ref stays where it was).

Snapping to integer pixel positions applies in Pixel-mode
documents (5.3.2), exactly like the Rectangle and Ellipse tools.

## Right-click for exact placement

Right-click the Ref Point button in the toolbox to open the **Place
Reference Point** popover:

- **X**, **Y** — coordinates in document units. Six-decimal
  precision.
- **Units** — read-only label showing the active display unit.
- **Place** button — drops the ref at the typed coordinates and
  dismisses the popover.

Useful for placing a ref at a known mathematical location — the
artboard centre, an intersection of guide angles, a calibration
mark at exact dimensions.

## What ref points are good for

A reference point is a **named position** in the document. It has
no fill, no stroke, no path — just an X, Y, and a name (Curvz
auto-generates a name from the coordinates by default). A handful
of common uses:

- **Pivot markers** — places to rotate or scale around when using
  the inspector's transform spinners. Drop a ref at the desired
  pivot, then transform.
- **Registration marks** — for icons that have to align with
  other artwork outside Curvz, drop refs at the alignment
  positions and use them for visual cross-checking.
- **Design notes** — if the icon's design has a "this is where
  the highlight should go" hint, leave a ref at that point.
  Future-you (or a collaborator) sees the marker and knows where
  to focus.
- **Construction guides** — when building geometry by hand,
  intermediate construction points are easier to place as refs
  than as full guides; they're points, not lines, so they don't
  cross the artboard.

## Refpoints don't export

Reference points live on a **dedicated Ref layer** that Curvz
treats as a "special layer" — visible in the editor, but stripped
from PNG exports and excluded from SVG exports. You can drop as
many refs as you need without worrying about contaminating the
delivered icon.

The exclusion is automatic and not user-configurable today. If
you genuinely need a marker to ship with the SVG (a registration
cross, a centre dot), draw it as a regular path on a regular
layer.

## Selecting and deleting refs

The Ref Point tool itself is for **placing** refs. To select,
move, or delete them after the fact:

- **Selection tool** — click a ref to select it, drag to move,
  marquee to multi-select. The Selection inspector section
  (5.4.1) shows X/Y for the selected ref(s) with no W/H (refs
  are points).
- **Layers panel** — every ref appears under the Ref layer; pick
  by name there, lock or hide the whole Ref layer to stop it
  responding to clicks.
- **Delete / Backspace** — removes the selected ref(s).

The Ref layer can be **locked** like any other. With it locked,
the Ref Point tool's clicks become no-ops — useful when you've
finished placing refs and want to stop accidentally adding more
while editing other artwork.

## Naming refs

Curvz auto-names every new ref from its coordinates — for a ref
at `(120.5, 84.0)` the name becomes something like
`120.500000_84.000000`. The auto-name updates whenever the ref
is moved.

If you want a meaningful name (e.g. `centre`, `top-mark`), set it
manually in the Selection inspector section's name field. Manual
names are preserved across moves.

## Where to next

For the inspector's per-refpoint controls — X / Y spinners,
Flip H/V buttons — see the **Selection** inspector section
(5.4.1). The "ref selected" branch of Selection has its own
mini-layout matched to refpoint geometry.

The companion concept is **guides** (5.3.3), which are *lines*
you can drag from the rulers. Refs are *points*; guides are
*lines*. They share the design intent — non-printing layout
helpers — but each fits a different mental model.

For *measurement* between two refs (or between a ref and a path
node), the **Ruler tool** (4.6.3) accepts refpoints as endpoints
just like path nodes.
