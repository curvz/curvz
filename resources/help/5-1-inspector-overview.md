# Inspector overview

The **Inspector** is the column down the right side of the window.
Every numeric, structural, and appearance control in Curvz lives
here. The toolbox draws shapes; the inspector inspects them.

The inspector adapts to context. With nothing selected on the canvas
it shows application- and document-level settings. Pick an object
and the panel reorganises around what that object is — its
position, its appearance, any tool-specific properties — and adds
rows that only make sense for that kind of node.

![The inspector with its three top-level groups visible](img/5-1-inspector-groups.png)

## Three groups

The inspector is a stack of collapsible **sections**, organised
under three top-level **groups**, listed top to bottom in the order
they appear in the panel:

- **Application** — your personal preferences for Curvz itself.
  These persist across every project and every launch. Six
  sections: **Appearance** (Dark/Light theme switch), **Startup**
  (reopen-last-project, recent-projects cap, show-rulers-by-default),
  **Editing** (undo history depth, tooltip delay), **Paths** (user
  library, user templates, log file, custom CSS overrides),
  **Boolean cleanup** (the quality slider that controls how
  aggressively boolean operations simplify their output), and
  **Warp** (defaults for new Warp instances).
- **Document** — settings owned by the active document. Top-level
  sections: **Metadata**, the **Theme** disclosure (which wraps
  **Canvas**, **Margins**, and **Grid**), and **Dimensions**.
  Snap and Measure are no longer inspector sections — both are
  toolbar popovers now (see 5.3.6 and 5.3.7 for redirects, or
  the **Measure** tool page, 4.6.3).
- **Object** — settings for the current selection. Up to eight
  sections, depending on what is selected: **Guides**,
  **Selection**, **Text** (replaces Selection for text nodes),
  **Blend**, **Warp**, **Node**, **Styling**, and **Shadow**.

There is no longer a **Project** group. Per-project settings
fragmented into application-tier and document-tier scopes during
the s148 motif → document migration; the empty group was retired
because collapsibles that open to nothing are bad UX.

Groups are themselves collapsible. Click a group header to fold or
unfold the whole tier; click a section header inside a group to
fold just that one. Curvz remembers which sections you had open
across sessions — open the panels you reach for often once, and
they stay where you left them.

## Following the data

The three-tier shape mirrors how data is stored, broadest scope to
narrowest. **Application** preferences live in
`~/.config/curvz/preferences.json` and apply to every project you
open. **Document** settings live inside each `.svg` file, in
Curvz's private metadata block. **Object** settings live on the
individual SVG element you have selected.

That layering matters when you copy a document into a different
project: theme bundles can be reapplied from the project's saved
themes (see 9.1), but the document's units, guides, grid, margins,
and dimensions come along with it. Likewise copying an object
between documents brings its appearance and node geometry — but
not the document framing around it. Application preferences are
user-level — they stay with you, not with any project, and are
unaffected by opening or copying work between projects.

## The Theme disclosure

The Document group is shorter than it once was. Three sections
that used to live there have moved or retired:

- **Snap** moved to a toolbar popover in s150 — right-click the
  Snap button (Q) for its options.
- **Measure** moved to a toolbar popover in s150 — right-click
  the Measure button (M) for its preferences. Behaviour for the
  tool itself is in 4.6.3.
- **Guides** moved to the Object group in s179 — guides are
  objects with a selection model, not a doc-level surface
  setting.

What stayed in Document scope is bundled differently now. The
**Theme** disclosure (5.3.8) wraps the three doc-level surfaces
that travel together as a saveable style preset: **Canvas**
(artboard / workspace / creation colours), **Margins**, and
**Grid**. The Theme bundle is what the Themes content panel
(9.1) saves and reapplies; the disclosure here is the live edit
surface for the active document's theme.

## Conditional sections

Several **Object** sections only appear for certain selections,
and the lede on each page says exactly when:

- **Text** (5.4.2) replaces the **Selection** section when the
  selection is a text node. For non-text selections an empty
  "Text" placeholder collapsible appears below Selection — its
  body is empty since the text-specific properties don't apply.
- **Blend** (5.4.3) appears when the selection is a blend
  container produced by Object → Blend.
- **Warp** is always visible per s155. When no Warp is selected
  it edits the AppPreferences template (the defaults a new Warp
  will inherit); when a Warp is selected it edits that instance.
  Warp has its own dedicated documentation, which is in
  preparation — the section will slot into the Object tier
  numbering when it ships.
- **Node** (5.4.4) appears when a single Bézier node is selected
  on a path.
- **Shadow** (5.4.6) appears for any node that can carry a drop
  shadow — paths, compound paths, groups, text, images, and clip
  groups.

If a section you expect is missing, check the selection. The
Object group adapts every time the selection changes.

## A note on the word "Appearance"

Three different surfaces in Curvz used to share or now share the
word *Appearance*, which can be confusing:

- **Application ▸ Appearance** — the Dark/Light theme switch for
  Curvz's own chrome (panels, dialogs, the inspector itself).
  GNOME-convention, app-tier.
- **Document ▸ Theme** disclosure — the bundle of per-document
  surface colours that the Themes content panel saves and reapplies.
  Doc-tier.
- **Object ▸ Styling** — fill, stroke, and opacity for the
  selected object. The section header reads **Styling** now (it
  was called *Appearance* until s148, when the rename made room
  for the Application-tier Appearance section without collision).
  Object-tier.

The help file for Object ▸ Styling is still numbered 5.4.5 with
the file name `5-4-5-appearance.md` for stable bookmarking, but
the in-product label is **Styling**.

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

The next pages walk each inspector section in detail, top to
bottom in the same order they appear in the panel:

- **Application group** — section preferences are documented in
  their tooltips; this manual chapter covers the document- and
  object-tier sections below.
- **Document group** — Metadata (5.3.1) through Dimensions
  (5.3.2), the Theme disclosure (5.3.8), and the retired Snap /
  Measure redirect pointers (5.3.6, 5.3.7).
- **Object group** — Guides (the Object-tier home of the section,
  documented separately), Selection (5.4.1), Text (5.4.2, replaces
  Selection for text nodes), Blend (5.4.3), Node (5.4.4), Styling
  (5.4.5), and Shadow (5.4.6).

If you are looking for the **Layers**, **Library**, **Swatches**,
**Styles**, **Documents**, or **Themes** panels, those are not
part of the inspector — they live below it under the **Content**
group, and have their own chapter (6).
