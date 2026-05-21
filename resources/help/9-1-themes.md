# Themes

A **Theme** is a saved bundle of document-level settings — the
non-physical "how this document is presented" attributes that
travel together as a coherent unit. Display unit, canvas surface
colours, guides, grid, margins, snap rules: one record, one
verb to apply, one verb to save. Build a theme once on the
document that feels right, then push that same feel into every
sibling document in the project (or carry it to another
project entirely via export).

Themes are a step above per-icon Style or per-colour Swatch.
Where styles bundle *appearance* and bind to objects, themes
bundle *settings* and apply to documents. They're the answer to
"every icon in this project should use the same grid spacing,
guide colour, and snap rules" — set it on one document, save
as a theme, apply across the rest.

This page is the conceptual doc — what a theme **is**, what it
captures, what the apply model means. For the **Themes panel**
(library list, apply targets, import / export), see **6.7
Themes**.

![A theme applied across two documents](img/9-1-themes-apply.png)

## What's bundled

A theme captures six sub-bundles of doc-level settings, in the
order the Inspector and the apply funnel walk them:

- **Display unit** — px / in / mm / pt. The unit the
  inspector spinners and rulers read in. Edited in
  **Dimensions** (5.3.2).
- **Canvas colours** — artboard, workspace, and creation, each
  with **separate values per app appearance mode** (Dark /
  Light). Capturing a theme writes the document's current
  values into the matching pair; the off-mode pair takes
  factory defaults. Capture the same document again in the
  other appearance mode to fill in the off-mode pair with
  your preferred values. Edited in the **Theme disclosure**
  (5.3.8).
- **Guides** — the guide colour plus the guide-layer
  visibility. Edited in **Object ▸ Guides** (5.3.3).
- **Grid** — enabled state, style (lines vs dots), spacing
  X/Y, offset X/Y, and the overlay colour. Edited in **Grid**
  (5.3.4).
- **Margins** — enabled state, the four insets, column and
  row counts and gaps, and the overlay colour. Edited in
  **Margins** (5.3.5).
- **Snap** — every snap rule (artboard, grid, guides, margins,
  objects, intersections, refpoints) plus the snap distance.
  Edited from the toolbar **Snap switch's** right-click
  popover (Snap left the inspector in s150).

What it does **not** capture:

- **Canvas geometry** — width, height, DPI, orientation,
  aspect ratio. Themes describe how a document is *presented*,
  not how big it is. Applying a theme never reshapes the
  canvas.
- **Document content** — paths, layers, images, text. Settings
  only, not the artwork.
- **Project-scoped libraries** — Swatches, Styles, and the
  theme library itself. Those live on the project, not on
  any one document.

## The apply model

Applying a theme writes its values into the targeted document
and the link ends there. Themes are **not bindings** — editing
the theme later does not retroactively repaint anything you
previously applied it to. The model is *apply and forget*. If
you've drifted from the theme on a particular document and want
to snap back, re-apply the same theme.

> **Apply is one-shot and not undoable.** Once a theme is
> applied, the targeted documents carry those values; there's
> no Ctrl+Z to put the old ones back. The **Themes panel**
> (6.7) shows a confirmation modal before apply runs as the
> safety net. Save your project before applying a theme to
> many documents if you want a separate restore point.

Library mutations (rename, duplicate, delete, import) **are**
undoable — Ctrl+Z reverts them. Apply is the only theme
operation that isn't.

The reason apply isn't undoable is the cost: snapshotting every
target document's pre-apply state into a single composite undo
entry would be expensive for an infrequent operation. The
confirmation modal trades that cost for an explicit "you sure?"
moment instead.

## Themes across projects

Themes live on the project. To carry a theme to another
project, **export** it from the Themes panel's overflow menu
(the kebab next to the [+] button) — you get a JSON pack file
that the receiving project can **Import** through the same
menu. Imported themes are added to the receiving library and
any name clashes get disambiguated automatically.

This is the pump for "I want every project I work on to start
with my standard grid + margins + snap setup." Build the theme
once, export it, and import it into every new project that
needs it. The next step up from per-project is to ship it as
part of a **Template** (see **2.2 Templates**) — a template
embeds the theme as part of the saved starting state.

## A typical workflow

1. Open a fresh document and tune it the way you want every
   document in the project to feel: pick the display unit, set
   up the grid, choose a guide colour, dial in the margins
   and snap rules, decide on the canvas surface colours.
2. Open the **Themes panel** (6.7) and click [+] to save the
   current document as a new theme. Give it a name like
   "Project standard."
3. To apply it elsewhere in the same project: click the theme
   row to mark it as the source, tick the target documents in
   the apply list, click **Apply**, confirm the modal.
4. To carry the preset to another project: click the [⋮]
   overflow menu in the panel and **Export…** — you get a
   JSON file you can drop on the other machine. In the
   destination project, [⋮] → **Import…** to pull it in.

## Where to next

- For the panel that drives the save and apply flow, see
  **Themes** (6.7).
- The **Inspector** chapter (5) is where every theme-affected
  setting lives day-to-day. The theme writes; the inspector
  edits.
- For *appearance* presets (fill, stroke, dash) that bind to
  objects rather than documents, see **Styles** (6.5).
- For *colour* presets shared across the project, see
  **Swatches** (6.4).
- For the per-document canvas surface colours (artboard /
  workspace / creation), see the **Theme disclosure** (5.3.8).
- For the Dark / Light app appearance switch (separate from
  theme bundles), see **Application ▸ Appearance** at the top
  of the inspector.
