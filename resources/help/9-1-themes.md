# Themes

A **Theme** is a saved bundle of document settings. Units, guide
colour, grid, margins, and snap behaviour all live inside one
theme record; apply a theme to a document and every one of those
settings is overwritten with the theme's values. Save a theme
once and you have a reusable preset you can carry across
documents and across projects.

Themes are a step above per-icon Style or per-colour Swatch.
Where styles bundle *appearance* and bind to objects, themes
bundle *settings* and apply to documents. They're the answer to
"every icon in this project should use the same grid spacing,
guide colour, and snap rules" — set it on one document, save as a
theme, apply across the rest.

![The Themes dialog with apply pane and library list](img/9-1-themes-dialog.png)

## Opening the dialog

**Project → Themes…** opens the Themes dialog. There is no
default keyboard shortcut. The dialog is one window with two
panes stacked vertically: **Apply** at the top, **Library** at
the bottom.

## ❶ The apply pane — source

The top section ❶ chooses *what to apply* via a pair of radio
buttons:

- **Saved theme** — pick one of the themes in the library below.
  This is the standard apply path: build a preset once, use it
  many times.
- **Live document** — pick any document in the project as the
  source. Curvz captures that document's current settings as if
  they were a theme and applies them. Useful for one-shot
  syncing without going through the save-as-theme step first.

Two dropdowns sit beside the radios — one for saved themes, one
for documents. Only the active mode's dropdown is enabled; the
inactive one dims out.

## ❷ The apply pane — targets

Below the source comes the **target list** ❷ — every document in
the project, each with a checkbox. Check the documents that
should receive the apply. Two convenience buttons cover the
common cases:

- **Select all** — every document in the project.
- **Select none** — clear all checks.

If you've picked Live document mode, the source document's row
in the targets list is disabled — applying a doc to itself is a
no-op.

## ❸ Apply

Hitting **Apply** ❸ writes the theme bundle to every checked
target document. The dialog also has a **Save current as
theme…** button, which opens a small naming prompt and adds the
active document's settings to the library — the create-from-doc
shortcut.

> **Apply is one-shot and not undoable.** Once a theme is
> applied, the targeted documents have those values; there's no
> binding back to the theme record. The dialog shows a hint
> reminding you of this. Re-opening the dialog and applying
> again is the way to "update bound docs" — they don't update
> automatically.

The reason apply is not undoable is the cost: snapshotting every
target's pre-apply state into a single composite undo entry would
be expensive for an infrequent operation. Save your project
before applying a theme to many documents if you want a safety
net.

## ❹ The library pane

The bottom section ❹ lists every theme you've saved. Each row
shows the theme's name and three icon buttons:

- **✎ Rename** — edit the name in place.
- **⎘ Duplicate** — copy the theme under a new id.
- **✕ Delete** — remove the theme from the library.

Library mutations (rename, duplicate, delete, import) **are**
undoable — Ctrl+Z reverts them. Apply is the only operation that
isn't.

A kebab menu on the right of the library row gives **Import…**
and **Export…**:

- **Import…** — load themes from a JSON pack file. Imported
  themes are added to the library; clashing names get
  disambiguated. Atomic undo over the whole import.
- **Export…** — write the entire user-tier library out as a
  JSON pack you can share or carry to another machine.

## What's bundled

A theme captures these document-level settings:

- **Display unit** (px / in / mm / pt) for the inspector and rulers.
- **Guide colour** and visibility.
- **Grid** — enabled, spacing X/Y, offset X/Y, colour, dots vs
  lines.
- **Margins** — enabled, top/bottom/left/right, columns / rows
  with gaps, colour.
- **Snap** — every snap rule: snap to artboard, grid, guides,
  margins, objects, intersections, refpoints; the snap distance.

What it does **not** capture:

- **Canvas geometry** — width, height, DPI, orientation.
  Themes describe how a document is *presented*, not how big it
  is. Applying a theme never reshapes the canvas.
- **Document content** — paths, layers, images. Settings only,
  not the artwork.
- **Project-scoped libraries** — Swatches, Styles, the theme
  library itself. Those live on the project, not the document.
- **Motif** — the workspace appearance from the inspector's
  Motif section is project-scoped (5.2.1), not document-scoped,
  so it isn't part of a per-document theme.

## A typical workflow

1. Build the first document the way you want every document in
   the project to feel: pick units, set up the grid, choose a
   guide colour, dial in the snap rules.
2. Open **Themes…**, click **Save current as theme…**, give it a
   name like "Project standard."
3. To apply it elsewhere, open Themes again, pick the theme as
   the source, check every other document in the targets list,
   click Apply.
4. To carry the preset to another machine, click the kebab in
   the library pane and **Export…** — you get a JSON file you
   can drop on the other machine and Import.

## Where to next

- The **Inspector** (chapter 5) is where every theme-affected
  setting lives day-to-day. The theme writes; the inspector
  edits.
- For *appearance* presets (fill, stroke, dash) that bind to
  objects rather than documents, see **Styles** (6.5).
- For *colour* presets shared across the project, see
  **Swatches** (6.4).
- For the workspace look (artboard / pasteboard colour, dark vs
  light), see **Motif** (5.2.1) — that's project-scope, not
  theme-scope.
