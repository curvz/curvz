# Troubleshooting

This page lists wrinkles and known limits in the current build,
with workarounds where they exist. Most are minor and don't
block real work; a few are worth knowing about up front so they
don't come as surprises.

## Where things live on disk

When something goes wrong, knowing where Curvz keeps its data
helps. On Linux:

- **Log file** — `~/.local/share/curvz/curvz.log`. Curvz writes
  diagnostic logging here. To inspect a fresh issue, clear the
  log first and reproduce: `> ~/.local/share/curvz/curvz.log`,
  then trigger the bug, then read the file.
- **Macros** — `~/.config/curvz/macros.json`. App-wide, shared
  across every project.
- **Library** (user) — `~/.config/curvz/library/`. SVGs you've
  added via the Library panel's Add button or by hand.
- **Palettes** (exported user palettes) — `~/.config/curvz/palettes/`.
  These show up in the Swatches kebab → Load palette submenu.
- **Style packs** (exported user styles) — `~/.config/curvz/styles/`.
  These show up in the Styles kebab → Load user pack submenu.
- **System library** (read-only) — `/usr/share/curvz/library/`.
  Shipped with the install if any artwork was bundled.

Project files (`.curvz` containers and their constituent SVGs)
live wherever you saved them — Curvz doesn't impose a project
location.

## Keyboard shortcuts that don't fire

Symptom: you press a Curvz hotkey and nothing happens.

Most common cause: **a spinner or text field has focus**, and
the keystroke went to it as input rather than to the canvas
controller. Click the canvas (or press `Esc`) to return focus,
then try the hotkey again.

Less common: the hotkey is **gated by a state check** that
isn't currently true. Examples:

- Boolean ops require exactly two paths selected; the menu
  items dim when the selection is wrong.
- Outline mode (`Ctrl + E`) refuses to switch in at extreme
  zoom levels (the drop-shadow buffer can crash). Zoom out and
  retry.
- Tool letters (`S`, `N`, `P` etc.) need the canvas to have
  focus; a spinner steals them as text input.

If a shortcut still doesn't fire after focus is correct and
state seems right, the log file is the next place to look —
many actions log a one-line message saying why they bailed.

## Drag-and-drop in the Layers panel

Symptom: you drag a row in the **Layers** panel and the drop
doesn't take, or the row lands somewhere unexpected.

Drag-and-drop on **compounds, deeply-nested groups, and
reference images** is occasionally inconsistent in the current
build. Two reliable workarounds for the cases where DnD
misbehaves:

- **Cross-layer moves** — right-click the row and use the
  **Move to layer ▸** submenu (top-level rows only; nested
  children must be released from their container first via
  the right-click's release verb).
- **Z-order reordering within a layer** — use the arrange
  hotkeys: `Ctrl + ↑` / `Ctrl + ↓` to step, `Ctrl + Shift +
  ↑` / `Ctrl + Shift + ↓` to jump to front / back.

Layer-to-layer reordering of layer headers themselves (the
top stripe of each layer card) works reliably. So does
dragging a plain path between layers. The unreliability is
mainly for container types being dropped *into* other
containers.

## Boolean operations producing artefacts

Symptom: Union, Subtract, or Intersect produces a result with
wrong geometry — extra spurious shapes, missing regions, jagged
edges.

Known limits in the current Bézier-clipping implementation:

- **Three-or-more-path Union in one go** can corrupt — even
  when the two-at-a-time variant works fine on the same
  geometry. **Workaround:** do the Union pairwise. Union the
  first two, select the result with the third, Union again.
- **Subtract and Intersect** are less battle-tested than Union.
  Verify on first use; small fractional-pixel nudges to one
  input often resolve edge-case clip failures.
- **Compound path inputs** are not supported. Split the
  compound first, run the boolean, re-compound if needed.
- **Open paths** are accepted but the geometry is unreliable.
  Close them or convert to filled regions first.

If the result is visibly wrong, undo, save the project under a
different name, and the saved file becomes a useful reproducer
for future fixes.

## currentColor paths look wrong on export

Symptom: an icon you expected to be theme-aware paints with a
fixed colour on the consuming side.

Likely cause: the path's fill or stroke is set to a literal
colour, not `currentColor`. Curvz substitutes a contrasting
display colour at draw time so `currentColor` paths are visible
on the canvas, which can mask the difference between a path
that's actually `currentColor` and a path that happens to be
black (or white, etc.).

To check: select the path, look at the inspector's **Styling**
section (5.4.5). The fill and stroke rows show the paint kind.
If it's a literal colour and you wanted symbolic, switch to
**currentColor** from the paint editor's type toggle.

For the full background on `currentColor` and symbolic icons,
see **currentColor & symbolic icons** (9.2).

## Inspector readouts look stale

Symptom: you change something on the canvas (paint colour,
binding, etc.) but the inspector still shows the old value
until you click another object and click back.

The inspector should refresh automatically on every relevant
state change. If it doesn't, that's a bug — the canvas and
inspector are connected by signals that occasionally miss a
fire path. Reproducing it in a simple case and recording the
log helps fix the underlying signal route.

In the meantime: clicking off and clicking back forces a
rebuild, as does opening any inspector section that was
previously collapsed.

## Snap behaviour feels off

Symptom: snap doesn't catch where you expected, or snap catches
in unexpected places.

Snap is configured in the inspector's **Snap** section (5.3.6) —
each snap target (artboard edges, grid, guides, margins,
objects, intersections, refpoints) has its own checkbox, plus a
master enable. The hotkey `Q` toggles the master enable.

If snap is firing wrong:

- Check which targets are enabled. The master switch can be on
  while only some targets are checked.
- The **snap distance** controls how close the cursor needs to
  be before a target wins. Too small and snap feels stingy;
  too large and snap fires from too far away.
- Some snap targets need their associated layer present.
  Snap-to-grid does nothing without a Grid layer; snap-to-
  margins does nothing without a Margin layer.

## Documents lost between machines

Symptom: opened a project on a different machine, some artwork
is missing.

The two things that don't travel with a `.curvz` project file:

- **Library items** — the Library panel content is
  machine-scoped. SVGs you placed *from* the library were
  inlined into the document at place time, so they survive,
  but the library *itself* is rebuilt from the new machine's
  user/system folders.
- **Macros** — also machine-scoped (`~/.config/curvz/macros.json`).
  Copy the file over to bring them along.

To carry **swatch palettes** or **style packs** across machines,
use the **Export** kebab actions in the Swatches and Styles
panels (6.4 / 6.5). Both produce portable files (`.gpl` for
palettes, `.json` for styles).

## File rename after save

Symptom: you'd like to rename a document or project after
creating it without going through "save as."

Document rename: double-click the document's name in the
**Documents** panel (6.6) or in the canvas tab strip, type a
new name, press Enter.

Project rename: not currently supported in-app. Save As to a new
file path, or rename the file in your file manager (Curvz will
pick it up on next open).

## When in doubt

- **Save first, experiment after.** Save your project with a
  versioned name (`my-project-v1.curvz`) before trying anything
  uncertain. Re-open the saved file if things go sideways.
- **Use Outline mode** (`Ctrl + E`) to see what's actually
  there. Filled artwork hides geometry in ways that can be
  confusing; outline doesn't lie.
- **Read the log.** `~/.local/share/curvz/curvz.log`. A single
  reproduction with a fresh log usually points at the cause.

If you hit something this page doesn't cover, the log file plus
the project file is the useful pair to capture for a bug report
— the log shows what Curvz thought it was doing, the project
file lets the bug be reproduced.

## Where to next

This is the last page in the manual. Browse the chapter list
on the left to revisit any topic; the in-app Shortcuts dialog
(`?` on the canvas) is the fastest way to look up a binding
mid-task.
