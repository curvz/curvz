# Projects and documents

Curvz organises your work as **projects**, each holding one or more
**documents**. A document is a single icon. A project is a folder
that gathers a family of icons together along with everything they
share — colour swatches, named styles, themes, motif preferences.

You always have exactly one project open. Opening a different
project closes the current one (after prompting if you have unsaved
changes).

![The project tree on disk and as it appears in Curvz](img/2-1-project-tree.png)

## Project shape on disk

A Curvz project is an ordinary directory. Curvz writes into it; you
can browse it with any file manager. The contents of a typical
project look like this:

```
MyIcons/
  project.json       — project-level state and settings
  swatches.json      — your custom colour swatches (if any)
  arrow-left.svg     — one document
  arrow-right.svg
  menu.svg
  ...
```

`project.json` carries everything that isn't per-document: colour
palettes, named styles, themes, the active motif, window state,
inspector preferences, and the document order. The `.svg` files are
the documents themselves — readable in any other vector tool, with
Curvz-specific metadata stored on the root `<svg>` element as
`data-curvz-*` attributes that other tools quietly ignore.

`swatches.json` is a sibling file rather than a sub-key of
`project.json` because swatches and palettes can be sizeable and
keeping them separate makes the project file diff-friendly under
version control.

There is no proprietary container, no sqlite database, no zipped
bundle — losing Curvz tomorrow leaves you with a directory of plain
SVG files and a JSON manifest you can read with any text editor.

## Documents within a project

The **Documents** strip across the top of the window holds one tab
per open document. Click a tab to switch documents; the canvas, the
layer stack, and the inspector all change to match. The colour
swatches, the styles, and the motif do not — those are
project-scoped and stay constant as you move between documents.

Three ways to add a document to the project:

- **File → New** (or **Ctrl+N**) opens the New Document dialog,
  which lets you pick a template, size, and starting grid for a
  fresh icon.
- **File → Import SVG** (or **Ctrl+I**) brings an existing SVG
  in as a new document, preserving its dimensions.
- **File → Import as Icon** (or **Ctrl+Alt+I**) does the same but
  rescales the long axis to 1000 units and converts solid fills
  and strokes to `currentColor`. Use this when bringing artwork in
  from non-Curvz sources to make it match your other icons.

Documents can be reordered by dragging tabs. Right-click any tab
to add a new document or remove the document from the project. To
rename or duplicate a document, use the inspector's **Documents**
panel (see 6.6) or its Metadata section (see 5.3.1). See
**Header & Document tabs** (3.1) for the full tab interactions.

## Saving

**File → Save** (**Ctrl+S**) writes the entire project — every
document, every JSON sidecar — back to its directory. There is no
per-document save: Curvz treats the project as one unit of work, so
a save is always whole-project.

A new project (one that has never been saved) prompts for a
location the first time you save. **File → Save As** writes the
project to a new location, leaving the original untouched on disk
but switching the in-memory project to the new directory. You end
up working in the copy, not the original.

## Autosave

Curvz autosaves continuously while you work, with one limit worth
knowing about.

The mechanism is simple: every change you make — moving a node,
nudging a value in the inspector, dragging a tab — schedules a save
that fires shortly after you stop. The save runs in the background;
there is no progress bar, no "Saved" toast, no perceptible pause.
Open your project directory in a file manager while you work and
you will see file timestamps tick forward as you edit.

Two refinements to that behaviour:

- **Saves coalesce.** A burst of rapid changes — scrubbing a
  spinner, dragging a slider, holding an arrow key — produce one
  save at the end of the burst, not one save per change. The save
  fires once the changes settle, typically within a fraction of a
  second of your last input.
- **Drags hold the save.** While you are actively dragging a node
  or handle, Curvz waits to save. The save runs as soon as you
  release the mouse, never mid-drag. This keeps the on-disk SVG
  consistent — you never get a snapshot of geometry caught
  half-way through a transformation.

The autosave only runs once a project has a directory on disk —
which means a brand-new, never-saved project does not autosave.
The first **Ctrl+S** establishes the directory; everything after
that is automatic. Curvz also performs a final save at quit, so
exiting normally will not lose work.

The case where unsaved changes can be lost is a hard crash or
force-quit on a project that has never been saved. Save once after
**File → New Project** and you have continuous autosave coverage
for the rest of the session.

## Project-scoped vs document-scoped

A useful mental model when looking at the inspector: settings under
the **Application** group are user preferences that persist across
projects (recent-projects cap, undo depth, ruler defaults), under
the **Project** group apply once across the project (Motif), under
the **Document** group apply to the active document only (Canvas,
Guides, Grid, Margins, Snap, Measure, Metadata), and under the
**Object** group apply only to whatever is currently selected on
the canvas.

The same split runs through Content (6) — Layers and the Inspector's
Object section follow the active document; Library, Swatches, and
Styles are project-wide.

## Where to next

- **Templates** (2.2) covers the New Document dialog and how
  templates seed a fresh document.
- **Import & export** (2.3) covers all the ways content moves into
  and out of a Curvz project.
- **Header & Document tabs** (3.1) covers the tab strip in detail.
