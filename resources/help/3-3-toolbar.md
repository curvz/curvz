# Toolbar

The toolbar is the column running down the left edge of the window.
It holds every drawing and navigation tool, plus the fill-and-stroke
well at the bottom. The toolbar is always visible and never changes
size — every tool is one click away.

## Anatomy

The buttons are arranged top-to-bottom in functional groups, with a
thin divider between groups:

- **Selection** and **Node** at the top — the two tools you spend
  most of your time in.
- The **drawing tools** in the middle — Pen, the shape tools
  (Rectangle, Ellipse, Line, Polygon, Spiral), the text tools
  (Text, Text on Path), and the placement tools (Reference points,
  Corner).
- The **utility tools** at the bottom — Eyedropper, Zoom, Ruler.

Below the tool list sits the **fill-and-stroke well** — two
overlapping squares showing the current fill (front) and stroke
(back) paints. Click either square to open the colour picker for
that paint. See **Color picker & paint editor** (3.7) for the
picker itself.

## Behaviour

- **Click** a button to switch to that tool. The current tool is
  shown by a pressed-in highlight on its button.
- **Hover** any button to see its tooltip. The tooltip names the
  tool, gives its single-key shortcut, and notes any right-click
  behaviour.
- **Right-click** the shape tools (Rectangle, Ellipse, Line,
  Polygon, Spiral) to open a dialog that places the shape at exact
  dimensions instead of dragging it out by hand. **Reference
  points** and **Zoom** also have right-click variants — see their
  per-tool pages in Chapter 4.

Switching tools never disturbs your selection. You can pick an
object with the Selection tool, switch to the Node tool to edit its
anchors, and switch back to Selection — the same object is still
selected.

## Single-key tool switches

Each tool has a single-letter shortcut. You can press the letter
when the canvas has focus and nothing is being edited (no active
text cursor, no in-progress pen path). The letter switches to the
tool just as if you had clicked its button.

The letters are mostly mnemonic: **R** Rectangle, **E** Ellipse,
**L** Line, **T** Text, **Z** Zoom. A few are positional — **S**
Selection, **N** Node — because the obvious mnemonic was already
taken by another tool.

For what each tool does, see the per-tool pages in **Chapter 4
Tools**. The toolbar is just the surface that hosts them.

### Keys

- `S` — Selection tool
- `N` — Node tool
- `P` — Pen
- `R` — Rectangle (also: with Selection tool + selection, pivot placement mode)
- `E` — Ellipse
- `L` — Line
- `G` — Polygon
- `W` — Spiral
- `T` — Text
- `U` — Text on Path
- `F` — Reference points
- `K` — Corner
- `I` — Eyedropper
- `Z` — Zoom
- `M` — Ruler
- `Q` — Toggle snap on / off (not a tool, but lives on the toolbar logic)
- `↑` / `↓` — Cycle to previous / next tool (only when nothing is selected)

These keys only fire when the canvas has focus. If a text field has
focus instead — for example an inspector spin button being typed
into — the same letter is treated as text input. Click the canvas
once, or press **Esc**, to return focus before pressing a tool key.
