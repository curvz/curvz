# Editing paths

Once a path exists, you'll spend most of your time *adjusting* it
rather than drawing it from scratch. Curvz splits creation and
adjustment between two tools — the **Pen** lays geometry down, the
**Node tool** refines it — and offers a stack of operations on top
that change a path's structure: open and close, split and join,
reverse direction, insert and delete nodes.

This section is the workflow leaf. The mechanics for each gesture
live in their tool's own section: see **Pen** (4.3) for laying
nodes, **Node tool** (4.2.2) for selecting and dragging them, and
**Node** (5.4.4) for the inspector spinners and node-type
dropdown. What follows here is how those pieces fit together when
you're tidying a path up.

![A path mid-edit, with the Node tool active and one anchor selected](img/8-1-path-editing.png)

## ❶ The four building blocks

Every path on a Curvz canvas is built from anchors connected by
Bézier segments. An **anchor** has a position in the document. A
**segment** is the curve between two consecutive anchors, shaped
by two control handles (one on each end). A **node type**
classifies the relationship between an anchor's two handles —
Symmetric, Smooth, Cusp, or Corner — which decides what happens
when you drag one handle and whether the other follows.

A path is **open** if it has explicit endpoints, or **closed** if
its last anchor connects back to its first by a closing segment.
The closing segment is just another segment: it can curve, it can
be straight, it has its own handles.

## ❷ The basic workflow

A typical path edit is:

1. **Pick the right tool.** Press `N` to switch to the Node tool
   if you're not already there. The Selection tool moves the
   whole path; the Node tool exposes its insides.
2. **Select what you're going to change.** Click an anchor to
   pick it, or marquee a region of anchors. Step through nodes
   on the active path with `Tab` and `Shift + Tab` if you'd
   rather use keys.
3. **Adjust.** Drag the anchor to move it; drag a handle to
   recurve. Use the inspector's **Node** section (5.4.4) for
   precise numeric edits.
4. **Re-classify if needed.** Tap a letter key — `A`/`M`/`C`/`K`
   — to switch the selected node's type. The handles re-arrange
   themselves to satisfy the new constraint.
5. **Commit and move on.** Edits coalesce on a 1.5-second timer,
   so a flurry of nudges becomes one undo step.

That's the steady-state loop. The structural ops below are how you
re-shape the path itself, not just its individual nodes.

## ❸ Inserting and deleting nodes

To add a node, **click on the curve** with the Node tool active. A
plain click on a segment — no modifiers — inserts a new anchor at
the click point, with handles set to keep the curve's shape
exactly. The new anchor takes the active node type from the type
dropdown. Useful when you need finer control around a tight bend.

To delete a node, select it and press `Delete` or `Backspace`. The
two segments that met at the deleted anchor merge into one, and
the path's curvature smooths across the gap.

Deleting an interior node always leaves a path with at least two
anchors (the segment count drops by one). Deleting a node from a
two-anchor path drops it to one — Curvz will let you do this, but
a one-anchor path renders nothing useful; it's usually a sign
the whole path should go.

## ❹ Open, close, split, join

Four structural verbs change a path's connectivity:

- **Close** — turn an open path into a closed one by adding the
  closing segment from end back to start. Press `J` with the
  Node tool active and one path selected.
- **Open** — turn a closed path into an open one. Same key, `J`,
  on a closed path. The break happens at the selected node, so
  pick the node where you want the path to start and end.
- **Split** — break a path into two separate path objects at the
  selected node. Press `B` with the Node tool. On a closed path,
  `B` opens it (one path, unbroken). On an open path with an
  interior node selected, `B` splits at that node.
- **Join** — combine two open paths into one. Select one node on
  each of two open paths (Shift-click across paths to add to the
  selection) and press `J`. The two paths become one open path.

The inspector's **Node** section also exposes Open here / Split
here as buttons when the conditions are met — see **Node**
(5.4.4).

## ❺ Reverse direction

Every path has a direction — first anchor to last (or for a
closed path, the order around the loop). Reversing it flips
which anchor is first, which affects:

- Boolean operations and clip masks, where direction can interact
  with even/odd fill rules.
- Text on Path, where the text follows the path's direction.
- Open and Split — both anchor "this side" / "that side" decisions
  by the path's direction.

Press `R` with the Node tool active to reverse. The currently
selected anchor stays selected, just with a different index.
The inspector's **Node** section shows the path's direction as a
small ↻ or ↺ arrow.

## ❻ Cleanup notes

A few habits that pay off:

- **Use the right node type.** Symmetric and Smooth nodes preserve
  curvature continuity automatically; if you pull a handle on a
  Cusp or Corner node, the other handle stays put. Picking the
  type that matches the geometry you want will save you from
  hand-balancing handles later.
- **Match counts before joining.** If you're going to blend two
  paths later (see **Blends**, 7.3), build them with the same
  number of nodes from the start — Equalize works but it adds
  geometry-preserving subdivisions in places you didn't choose.
- **Lines start as all-corner.** The Line tool produces paths
  where every segment is straight and every node is a Corner. If
  you turn a Line-tool path into a curve later, you'll be
  switching its node types from Corner to Smooth or Symmetric
  one by one. The Pen tool gives you Corner or Smooth at the
  point of creation, which is usually the better starting point
  for curved geometry.

## Where to next

- For *creating* paths from scratch, see **Pen** (4.3) and the
  shape tools at **Rectangle** (4.4.1) onwards.
- For per-anchor numeric editing, see **Node** (5.4.4).
- For path-level operations that *combine* paths or *derive* new
  ones — Union, Compound, Offset, Expand Stroke — see
  **Boolean operations** (8.2) and **Compound & derived** (8.3).
- For interpolating between two paths, see **Blends** (7.3).

### Keys

These bindings only fire when the canvas has focus. The arrow keys
work with the Selection tool active too — they nudge the whole
selection rather than a single node.

- `N` — switch to the Node tool.
- `P` — switch to the Pen tool.
- `Tab` / `Shift + Tab` — cycle through nodes on the current path
  (Node tool active). **`Ctrl + Tab` / `Ctrl + Shift + Tab`**
  cycles through *documents*, not nodes — this differs from some
  other vector editors. Plain Tab is the per-node cycle here.
- `A` / `M` / `C` / `K` — set selected node(s) to Symmetric /
  Smooth / Cusp / Corner.
- `J` — close an open path, open a closed path, or join two open
  paths (one node selected on each).
- `B` — break the path at the selected node (split open paths,
  open closed paths).
- `R` — reverse the path's direction.
- `Delete` / `Backspace` — delete the selected node (Node tool)
  or the selected object (Selection tool).
- `←` / `→` / `↑` / `↓` — nudge by 2 screen pixels;
  `Shift + arrow` 8 px; `Alt + arrow` 32 px. `Ctrl + arrow` is
  reserved for arrange (z-order).
- **Plain click on a curve segment with the Node tool** inserts a
  new anchor at the click point — no modifier required.

The full master cheatsheet for every tool is at
**Keyboard shortcuts** (11.2).
