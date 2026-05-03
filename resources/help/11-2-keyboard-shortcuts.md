# Keyboard shortcuts

This is the master reference for every keyboard shortcut in
Curvz. Per-page **`### Keys`** subsections in earlier chapters
cover shortcuts in their workflow context; this page is the flat
list, organised by category.

A Curvz convention worth knowing: the in-app **Shortcuts dialog**
(`?` with no modifier on the canvas) shows a similar cheatsheet
in a popup. That dialog and this page cover the same bindings.
The page survives offline; the dialog is faster mid-task.

## File and project

- `Ctrl + N` — new project.
- `Ctrl + O` — open project.
- `Ctrl + S` — save project.
- `Ctrl + Shift + S` — save project as.
- `Ctrl + Tab` / `Ctrl + Page Down` — next document in project.
- `Ctrl + Shift + Tab` / `Ctrl + Page Up` — previous document.

## Edit

- `Ctrl + Z` — undo.
- `Ctrl + Shift + Z` / `Ctrl + Y` — redo.
- `Ctrl + X` — cut.
- `Ctrl + C` — copy.
- `Ctrl + V` — paste.
- `Ctrl + D` — duplicate.
- `Ctrl + A` — select all.
- `Ctrl + Alt + D` — Step and Repeat.
- `Delete` / `Backspace` — delete selection or selected guides.
- `Esc` — clear selection or exit current mode.

## Tools

These work with the canvas focused and no modifier.

- `S` — Selection tool.
- `N` — Node tool.
- `P` — Pen tool.
- `R` — Rectangle (or pivot mode if Selection is active and
  has a selection).
- `E` — Ellipse.
- `L` — Line.
- `G` — Polygon.
- `W` — Spiral.
- `T` — Text.
- `U` — Text on Path.
- `F` — Reference points.
- `K` — Corner.
- `I` — Eyedropper.
- `Z` — Zoom.
- `M` — Ruler.
- `↑` / `↓` (with no selection) — cycle previous / next tool.

## View

- `Ctrl + R` — toggle rulers.
- `Ctrl + E` — toggle outline mode.
- `+` / `=` / `Ctrl + +` — zoom in.
- `-` / `Ctrl + -` — zoom out.
- `0` / `Ctrl + 0` — zoom to fit (artboard).
- `1` / `Ctrl + 1` — zoom to 100% (1× of fit).
- `2` / `Ctrl + 2` — zoom to 200% (2× of fit).
- `3` / `Ctrl + 3` — zoom to selection.
- `Ctrl + Shift + 0` — zoom to fit all objects.
- `Q` — toggle snap.
- `F1` / `Alt + ?` — open this manual.
- `?` — open the in-app Shortcuts dialog.

## Selection and arrangement

- `←` / `→` / `↑` / `↓` — nudge selection by 2 px.
- `Shift + arrow` — nudge by 8 px.
- `Alt + arrow` — nudge by 32 px.
- `Ctrl + ↑` — bring forward (z-order).
- `Ctrl + ↓` — send backward.
- `Ctrl + Shift + ↑` — bring to front.
- `Ctrl + Shift + ↓` — send to back.
- `Ctrl + G` — group.
- `Ctrl + Shift + G` — ungroup.

## Align & Distribute

These fire only when the Selection tool is active and 2+ objects
are selected. See **Align & Distribute** (7.4) for the operation
detail and the anchor-click idiom.

- `Ctrl + Alt + L` — Align Left.
- `Ctrl + Alt + H` — Align Center Horizontal.
- `Ctrl + Alt + R` — Align Right.
- `Ctrl + Alt + P` — Align Top (mnemonic: toP).
- `Ctrl + Alt + M` — Align Center Vertical (Middle).
- `Ctrl + Alt + B` — Align Bottom.
- `Ctrl + Alt + click` — toggle align anchor on a selected
  object (canvas-side, not a window hotkey).

The two distribute ops (Distribute Horizontally, Distribute
Vertically) are menu-only; reach them through **Align**.

## Path operations

- `Ctrl + Shift + U` — Union.
- `Ctrl + Shift + E` — Subtract (Exclude).
- `Ctrl + Shift + I` — Intersect.
- `Ctrl + 8` — make compound path.
- `Ctrl + Shift + 8` — split compound path.
- `Ctrl + 7` — make clip group.
- `Ctrl + Alt + 7` — release clip.
- `Ctrl + Shift + O` — Offset Path…
- `Ctrl + Shift + X` — Expand Stroke.
- `Ctrl + Alt + T` — Convert Text to Path.
- `Shift + U` (no Ctrl) — release Text on Path.

## Node tool keys

These fire only when the **Node tool** is active and the canvas
has focus. They re-classify, restructure, or step through nodes
on the current path.

- `A` — set node type to Symmetric.
- `M` — set node type to Smooth.
- `C` — set node type to Cusp.
- `K` — set node type to Corner.
- `R` — reverse path direction.
- `J` — close (open path) / open (closed path) / join two open
  paths (with one node selected on each).
- `B` — break the path at the selected node.
- `Tab` / `Shift + Tab` — next / previous node on the active
  path.
- **Plain click on a curve segment** — insert a new anchor at
  the click point.

## Selection tool keys

These fire only when the **Selection tool** is active.

- `R` (with selection) — pivot placement mode.
- `Esc` — exit pivot mode or clear selection.

## Macros

- `Ctrl + M` — run the current macro on the selection.
- `Ctrl + Shift + M` — open the Macro Manager.

## Style binding

- `Ctrl + Shift + B` — unbind selection from any style it's
  bound to.

## Modifier conventions

While dragging or operating with a tool, modifier keys often
constrain or extend behaviour:

- **Shift** — constrain to axis or proportional resize.
- **Alt** — drag from centre, alternate-mode click (e.g.
  Alt-click a swatch chip to apply to stroke rather than fill).
- **Ctrl** — used for multi-select on rows (Layers, Swatches,
  Documents).
- **Space + drag** — pan the canvas. (Hold Space, drag with the
  mouse; release Space to return to the active tool.)

## Quirks worth knowing

A few bindings deviate from common other-vector-editor
conventions and are worth flagging:

- **`Ctrl + Tab` switches documents, not nodes.** Other editors
  use Ctrl+Tab for cycling within a path's nodes. Curvz uses
  plain `Tab` for that and reserves `Ctrl + Tab` for the
  document-tab strip (see **Header & Document tabs**, 3.1).
- **`?` is the Shortcuts dialog, not the manual.** `F1` opens
  the manual. The Shortcuts dialog is a faster-to-pop quick
  reference.
- **Plain digits zoom.** `1` / `2` / `3` / `0` zoom even
  without a Ctrl modifier, as long as a spinner doesn't have
  focus (which would interpret digits as input). Both forms
  are wired so you can use whichever your hand prefers.
- **Tool letters work even with the toolbox not focused.** The
  canvas controller catches them at capture phase. If a
  spinner has focus, click the canvas first or press `Esc` to
  return focus before pressing a tool letter.

## Where to next

- Per-page **`### Keys`** sections in **Selection tool** (4.2.1),
  **Node tool** (4.2.2), and **Editing paths** (8.1) cover the
  shortcuts in workflow context.
- The in-app **Shortcuts dialog** (`?`) covers the same set in
  popup form.
- For why these bindings can deviate from `set_accels_for_action`
  defaults, see **Troubleshooting** (11.3) — the relevant
  detail is that every shortcut is wired explicitly in the
  capture-phase keyboard controller rather than via GTK
  accelerator dispatch.
