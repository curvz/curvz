# Inspector overview

The **Inspector** is the column down the right side of the window.
Every numeric, structural, and appearance control in Curvz lives
here. The toolbox draws shapes; the inspector inspects them.

The inspector adapts to context. With nothing selected on the canvas
it shows project- and document-level settings. Pick an object and
the panel reorganises around what that object is — its position,
its appearance, any tool-specific properties — and adds rows that
only make sense for that kind of node.

![The inspector with its three top-level groups visible](img/5-1-inspector-groups.png)

## Three groups

The inspector is a stack of collapsible **sections**, organised
under three top-level **groups**:

- **Project** — settings shared by every document in the open
  project. Currently one section: **Motif** (theme, artboard,
  workspace).
- **Document** — settings owned by the active document. Seven
  sections: **Metadata**, **Canvas**, **Guides**, **Grid**,
  **Margins**, **Snap**, **Measure**.
- **Object** — settings for the current selection. Up to six
  sections, depending on what is selected: **Selection**, **Text**,
  **Blend**, **Node**, **Appearance**, **Shadow**.

Groups are themselves collapsible. Click a group header to fold or
unfold the whole tier; click a section header inside a group to
fold just that one. Curvz remembers which sections you had open
across sessions — open the panels you reach for often once, and
they stay where you left them.

## Following the data

The three-tier shape mirrors how data is stored. **Project**
settings live in `project.json` at the project root. **Document**
settings live inside each `.svg` file, in Curvz's private metadata
block. **Object** settings live on the individual SVG element you
have selected.

That layering matters when you copy a document into a different
project: motif and theme will switch to the new project's, but the
document's units, guides, grid, and margins come along with it.
Likewise copying an object between documents brings its appearance
and node geometry — but not the document or project framing
around it.

## Conditional sections

Three of the **Object** sections only appear for some node types,
and the lede on each page says exactly when:

- **Text** (5.4.2) appears when the selection is a text run.
- **Blend** (5.4.3) appears when the selection is a blend container
  produced by Object → Blend.
- **Shadow** (5.4.6) appears for any node that can carry a drop
  shadow — paths, compound paths, groups, text, images, and clip
  groups.

If a section you expect is missing, check the selection. The
Object group adapts every time the selection changes.

## Editing convention

Every editable field in the inspector follows the same rules:

- **Numeric spin buttons** accept typed values and arrow-key
  nudges, respect the document's display unit, and commit on
  Enter or focus loss.
- **Text entries** for names show a checkmark icon when there is
  a pending edit. Press Enter or click the icon to commit.
- **Colour swatches** open the colour picker on click. See **Color
  picker & paint editor** (3.7) for the picker itself.
- **Edits coalesce on undo.** A run of nudges or edits to the
  same property within a short window collapse to a single Ctrl+Z
  step, so you do not have to undo every tick of an arrow key.

The inspector never edits the selected object directly. Every
change goes through the canvas's command path, so undo, redo, and
multi-object broadcast all work the same way they would for a
canvas-driven edit.

## Resizing and hiding

Drag the boundary between the canvas and the inspector to widen
or narrow the panel. The minimum is 280 pixels — narrow enough to
fit on a small screen, wide enough that the spin buttons and
swatches do not crowd. The position is remembered across
sessions.

## Where to next

The next twelve pages walk each inspector section in detail, top
to bottom in the same order they appear in the panel:

- **Project group** — Motif (5.2.1).
- **Document group** — Metadata (5.3.1) through Measure (5.3.7).
- **Object group** — Selection (5.4.1) through Shadow (5.4.6).

If you are looking for the **Layers**, **Library**, **Swatches**,
**Styles**, or **Documents** panels, those are not part of the
inspector — they live below it under the **Content** group, and
have their own chapter (6).
