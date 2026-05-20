# Workspace tour

Curvz puts every editing surface you need in front of you at once — no
floating palettes, no hidden modes. This page walks you around the
window so the rest of the manual has names to point at.

![The Curvz workspace, with each region numbered for reference](img/1-3-workspace-overview.png)

## A bird's-eye view

The window is split into six regions, arranged the same way every
time you open Curvz:

- **❶ Title bar** runs across the top. It holds the app logo, your
  open document tabs, and the application menu.
- **❷ Toolbox** runs down the left edge, one icon per tool.
- **❸ Context bar** is the thin strip directly above the canvas. It
  changes contents as you switch tools.
- **❹ Canvas** sits in the middle, framed by rulers along its top
  and left edges. This is where you draw.
- **❺ Inspector** runs down the right side. It is the home of every
  numeric and structural control — properties, layers, library,
  swatches, styles, themes, documents.
- **❻ Status bar** runs across the bottom and tells you where you
  are and what is selected.

The rest of this tour names everything in those regions.

## ❶ Title bar

The far-left logo is a button. Click it to open the About dialog.
Right of the logo is the **Documents** label and a vertical
separator, followed by the open-document tabs. New documents appear
as new tabs; clicking a tab makes that document active. Right-click
the **Documents** label to add a new document to the project.

The far-right **☰** button opens the main application menu. The
submenus are **File, Edit, Arrange, Align, Path, View, Navigate**, and
**Developer**, followed by leaf items for **Help**, **Keyboard
Shortcuts**, and **Quit**. Every top-level action lives there, with its
keyboard shortcut shown beside it.

## ❷ Toolbox

The toolbox holds every drawing and navigation tool, grouped into four
sections separated by thin horizontal rules:

- **Selection** at the top — the Selection and Node tools, which pick
  objects and edit their anchor points.
- **Creation** — the tools that put new geometry on the canvas: Pen,
  the four primitive shape tools, Reference Point, the two text tools,
  Polygon/Star, and Spiral.
- **Transforms** — buttons that operate on the current selection rather
  than creating new objects: Align, Blend, Boolean, Corner, Step &
  Repeat, Warp. Most of these are one-shot operation buttons; Corner is
  the exception, a toggle tool you stay in to apply round, chamfer, or
  inverse corner treatments.
- **Utility** at the bottom — Eyedropper, Measure, and Zoom. Selection
  and navigation, no geometry effect.

Every tool has a single-key shortcut you can press to switch to it when
the canvas has focus. The full list:

- **Select (S)** and **Nodes (N)** — pick objects and edit their
  anchor points.
- **Pen (P)** — draw freeform Bézier paths node by node.
- **Rectangle (R)**, **Ellipse (E)**, **Line (L)** — drag-out shape
  tools. Right-click any of these to place one at exact dimensions
  via a dialog.
- **Reference Point (F)** — drop a non-printing reference point.
  Right-click to place precisely.
- **Text (T)** and **Text on Path (U)** — draw a text box, or attach
  an existing text run to a path.
- **Polygon / Star (G)** and **Spiral (W)** — parametric shape tools.
  Right-click to configure sides, angle, and similar.
- **Corner (K)** — apply round, chamfer, or inverse corner treatments
  to selected nodes.
- **Eyedropper (I)** — sample a colour from anywhere in the canvas.
- **Measure (M)** — measure the distance between two points.
- **Zoom (Z)** — drag a marquee to zoom into a region. Right-click to
  set a precise zoom level.

Hover any tool button for its full tooltip, including any right-click
behaviour.

Below the tool list, near the bottom of the toolbox, the **fill and
stroke well** shows the active paint pair. Click either square to
edit the corresponding paint with the popover editor.

## ❸ Context bar

The thin strip above the canvas is the context bar. The left side
always shows a glyph and the name of the current tool, so you have a
visual confirmation of what is active. The centre fills with a hint
strip — modifier keys and short tips for the active tool. The
Rectangle tool, for example, shows hints like "Drag to draw" and
"Shift + drag — constrain to square."

The context bar is read-only — it's a glanceable cheat-sheet, not a
settings panel. Tool parameters like corner radius, side count, or
font size live on the inspector's Object section (5.4) once you
have placed something.

The context bar changes whenever you switch tools. See **Context
bar** (3.4) for the full per-tool hint reference.

## ❹ Canvas

The canvas is the drawing surface. The white rectangle inside it is
the **artboard** — your document's printable extent. Anything outside
the artboard is still part of the document but sits on the
**pasteboard**, useful as scratch space.

Curvz uses a Y-up coordinate system with the origin at the
artboard's lower-left corner. The rulers along the top and left
edges reflect this: X grows rightward, Y grows upward, both starting
at zero in the bottom-left. If you prefer a different origin,
right-click the small square where the rulers meet to open the
ruler origin popover (or drag from the corner outward to set the
origin visually). See **Canvas, rulers & corner** (3.6) for the
full set of corner-square interactions.

To toggle the rulers off, press **Ctrl+R** or use **View → Rulers**.

The canvas has two render modes:

- **Preview** mode shows fills, strokes, gradients, blurs, and drop
  shadows — the document as it will export.
- **Outline** mode shows only path skeletons. Use this for precision
  editing on busy artwork.

Toggle between them with **Ctrl+E** or **View → Outline Mode**. The
status bar shows which mode you are in.

## ❺ Inspector

The inspector is the column on the right and is the bulk of Curvz's
surface area. Every section is a collapsible row — click a section
header to open or close it. Sections you have open survive across
sessions.

The top of the inspector holds the **Properties** panel, which
adapts to whatever you have selected:

- With nothing selected, it shows document properties — size, units,
  and document-level settings.
- With one object selected, it shows that object's geometry,
  appearance, and any tool-specific properties.
- With multiple objects selected, it shows shared properties across
  the selection.

Below Properties is the **Content** group, which collapses several
related panels:

- **Layers** — the layer stack for the active document, including
  ordering, visibility, and lock controls.
- **Library** — the project's asset library: SVG snippets you can
  drop onto the canvas.
- **Swatches** — the project's named colour swatches.
- **Styles** — the project's named appearance styles (combined fill,
  stroke, effect presets).
- **Themes** — the project's themes, which remap swatches and styles
  document-wide.
- **Documents** — thumbnails for every document in the project.
  Double-click a thumbnail to switch to it; right-click to rename,
  duplicate, or delete.

You can drag the boundary between the canvas and the inspector to
resize the inspector. Curvz remembers the position across sessions.

## ❻ Status bar

The status bar runs across the bottom of the window and reports the
following, left to right:

- **Cursor position** in document units, updating as you move the
  pointer over the canvas.
- **Zoom percentage**.
- **Document units** (px, mm, in, etc.).
- **Object and node counts** for the current selection.
- **Render mode** — `Preview` or `Outline`.
- **Project name** (or `unsaved` if the project has never been
  saved).
- **Active document name**, right-aligned at the far edge.

The status bar is read-only — every value here is settable somewhere
else in the UI. It is meant to be glanced at, not interacted with.

## Where to next

If this is your first time in Curvz, two next steps work well:

- **Projects and documents** (see 2.1) explains what the project
  wrapper around your documents is, and how saving works. Worth
  reading before you commit serious work.
- **Tools overview** (see 3.1) gives a one-paragraph rundown of every
  tool in the toolbox, with pointers into the deep-dive topics.

Both are short. After that, the rest of the manual fans out by topic
and you can pick what is relevant to the work in front of you.
