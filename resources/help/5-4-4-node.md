# Node

The **Node** section is the precision-editing pane for a single
Bézier node. It appears whenever you have a single node selected
on a path, with the **Node tool** (N) active or after picking a
node from the canvas.

The header shows the node's index in the form `Node  3 / 12` —
useful when stepping through nodes with Tab — and the body is a
small grid of three two-axis spinners covering the node's anchor
and its two control handles, plus a node-type dropdown and a
panel of path-state actions.

![The Node section showing IN/Node/OUT spinner rows, Type dropdown, and path actions](img/5-4-4-node-section.png)

## ❶ Coordinate grid

Three rows under **X** and **Y** column headers:

- **IN** — the inbound handle (`cx1`, `cy1`).
- **Node** — the anchor itself (`x`, `y`).
- **OUT** — the outbound handle (`cx2`, `cy2`).

Editing the **Node** row moves the anchor and carries both handles
along — the relative geometry of the node's curve segment stays
intact. Editing **IN** or **OUT** moves just that handle. All six
spinners respect the document's display unit and the active ruler
origin.

> **Note:** the **IN** and **OUT** row labels are clickable
> buttons. Click either one to **retract** that handle — the
> handle's `cx`/`cy` are set to the anchor's `x`/`y`, producing
> a perfectly straight segment on that side of the node. The
> retraction is one undo step. For a Symmetric node, clicking
> either label retracts both handles at once, since the
> symmetric constraint would otherwise force one to follow the
> other.
>
> The same operations have keyboard shortcuts: **Ctrl + ←**
> retracts the IN handle, **Ctrl + →** retracts the OUT handle.
> Both work across the entire multi-node selection in a single
> undo step. Symmetric nodes retract both handles regardless of
> which key was pressed, matching the label-click behaviour.

## ❷ Type

A dropdown with four options — **Symmetric ◆**, **Smooth ◇**,
**Cusp □**, **Corner ○**. Each enforces a different relationship
between IN and OUT:

- **Symmetric** (◆) — handles are mirror images: same length, same
  angle through the anchor. Produces a perfectly smooth curve with
  matching tension on both sides.
- **Smooth** (◇) — handles share an angle (still collinear through
  the anchor) but their lengths are independent. The curve passes
  smoothly through but tension can differ either side.
- **Cusp** (□) — handles are independent in both length *and*
  angle. The curve can change direction sharply at the node.
- **Corner** (○) — handles are independent like Cusp, but the
  visual handle decoration on the canvas reads as a straight-line
  joint. Useful for hard architectural corners.

Changing the type can move handles to satisfy the new constraint
(e.g. switching to Symmetric will force one handle to mirror the
other). Edit by dropdown or use the Node-tool letter keys below.

## ❸ Path actions

Below the type dropdown are two conditional buttons that reflect
the current path state:

- **Open here** — appears when the path is closed AND has at least
  two nodes. Click to open the path at this node, breaking the
  closing segment so the path now starts and ends here.
- **Split here** — appears when the path is open, has at least
  three nodes, and the selected node is **interior** (not the
  first or last). Click to split the path into two separate
  objects at this node.

Both actions are undoable as single steps. The **Path** row at the
bottom of the section is the inverse — it toggles open/closed as a
single shape, not splits.

## ❹ Path

A small horizontal panel at the bottom of the section, summarising
the path that owns this node:

- **Closed / Open** toggle — flip the path's closure state. A
  closed path's last node connects back to the first; an open
  path has explicit endpoints.
- **Direction indicator** — a small ↻ (clockwise) or ↺
  (counter-clockwise) arrow, computed from the path's signed area.
  Hover for the matching reverse-action tooltip.
- **Reverse** button — flip the path's direction. The selected
  node index re-maps to keep the same anchor selected.
- **Node count** — a quiet `12 nodes` readout for orientation.

The Closed / Open toggle is the same operation as J in the Node
tool (when only one path is involved). The Reverse button matches
R in the Node tool.

## When the Node section is empty

The section header still appears when there is no node selected,
but the body is blank. Pick a node on the canvas with the Node
tool to populate it — clicking a node, marquee-selecting one node,
or stepping with Tab all work.

For multi-node selections, edits applied via Type apply to **every
selected node** on the same path; coordinate edits apply only to
the primary (most-recent) node.

## Where to next

The Node tool itself is documented at **Node tool** (4.2.2) —
including the canvas-side gestures (drag anchors and handles to
recurve, Tab cycle, click a curve segment to insert a node). The
keys here mirror the tool's behaviour but are sometimes faster
than reaching for the dropdown.

For *path-level* operations like Step and Repeat, Boolean
operations, or compound paths, see **Working with objects** (7) and
**Path operations** (8).

### Keys

These bindings only fire when the canvas has focus and the **Node
tool** (N) is active.

- `A` — set node type to **Symmetric**.
- `M` — set node type to s**M**ooth.
- `C` — set node type to **Cusp**.
- `K` — set node type to corner (**K**ink).
- `R` — reverse the path.
- `J` — close the path if open, or open the path if closed. With
  one node selected on each of two open paths, joins them into a
  single open path.
- `B` — break the path at the selected node. Open path → splits
  into two objects. Closed path → opens at this node (one path,
  unbroken).
- `Tab` / `Shift + Tab` — cycle through nodes on the current
  path.
- `Delete` / `Backspace` — delete the selected node. The path's
  curvature smooths across the gap. Multi-node selections delete
  every selected node in one composite undo step.
- `←` / `→` / `↑` / `↓` — nudge the selected node by about 2
  screen pixels. `Shift + arrow` is 8 px, `Alt + arrow` is 32 px.
- `Ctrl + ←` — retract the IN handle of every node in the
  multi-selection to its anchor. Symmetric nodes retract both
  handles. Composite undo.
- `Ctrl + →` — retract the OUT handle of every node in the
  multi-selection. Symmetric nodes retract both handles.
  Composite undo.
- `Ctrl + ↑` / `Ctrl + ↓` — pass through to arrange actions
  (Ctrl-arrow on the horizontal axis is bound to retraction
  inside the Node tool).

The letter keys also accept the lowercase form (`a`, `m`, `c`,
`k`, `r`, `j`, `b`). If a spin button has focus, letters type into
the spinner; click the canvas once or press **Esc** to return
focus.
