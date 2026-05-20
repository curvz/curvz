# Content overview

The right side of the Curvz workspace is the **Content pane** — six
stacked panels that hold the parts of your project that aren't a
single icon. Layers organise what's inside the active document.
Library, Swatches, Styles, and Themes hold reusable assets shared
across the project. Documents lists every icon in the project and
lets you switch between them.

This chapter walks each panel in turn. They sit one above the other
in a single scrollable column, each with a collapsible header so you
can hide the ones you aren't using and focus on the ones you are.

![The six Content panels stacked in the right pane](img/6-1-content-panels.png)

## ❶ Layers — what's inside the active document

The **Layers** panel ❶ shows the structure of the icon you are
currently editing: every path, group, guide, and reference image,
arranged the way the renderer stacks them. It also hosts a small
**Macros** strip at the top for one-click runs of starred macros.

Layers are document-scoped. Switching to a different document in the
project replaces the layer list with that document's stack.

See **Layers** (6.2).

## ❷ Library — reusable artwork

The **Library** panel ❷ is a gallery of SVGs you can drop into any
document. Curvz scans two locations: a system folder shipped with
the app and a user folder in your home directory. Items group into
categories by their parent folder name, so you can organise as
deeply or as shallowly as you like.

The library is shared across every project on the machine, not
saved with any one project.

See **Library** (6.3).

## ❸ Swatches — named colours

The **Swatches** panel ❸ is your project's named colour library.
Add colours from the picker, organise them into palettes, and apply
them to selections with a click. A recents strip at the top tracks
the last colours you used. Palettes can be exported and imported as
`.gpl` files (the GIMP / Inkscape format) so you can share or reuse
them across projects.

Swatches are project-scoped. Each project carries its own swatch
library, saved in `project.json`.

See **Swatches** (6.4).

## ❹ Styles — named fill + stroke + dash combinations

The **Styles** panel ❹ goes one level above swatches: it stores a
*combination* of fill, stroke, stroke width, and dash settings as a
single named entry. Apply a style to a selection and every property
in the bundle is set in one step. Edit the style afterward and every
icon bound to it updates automatically.

Styles split into two tiers — built-in (read-only) and user — and
can be grouped into categories. Like swatches, the user tier can be
exported and imported as JSON packs.

See **Styles** (6.5).

## ❺ Themes — doc-level surface presets

The **Themes** panel ❺ saves a document's per-document surface
settings — Canvas colours, Margins, Grid — as a single named
preset, and applies that preset to one or more documents in the
project. Use it to keep a consistent feel across an icon family,
or to flip a set of icons between editing-time presentations
without altering the artwork.

Themes are project-scoped, saved in `project.json`. User themes can
be exported and imported as JSON packs.

See **Themes** (6.7).

## ❻ Documents — every icon in the project

The **Documents** panel ❻ at the bottom is the project's icon
gallery. It has two tabs: **Project**, which lists the documents
saved in this project, and **System**, which browses your operating
system's installed icon themes (handy for picking up reference art
or comparing your work against a stock icon).

Click any document to switch to it. The header has buttons to add a
new document, duplicate the active one, remove one, or clear them
all. A search field filters by name.

See **Documents** (6.6).

## Project vs document scope

It's worth keeping clear in mind which panels carry which kind of
state:

- **Document-scoped** — Layers. The list changes when you switch
  documents.
- **Project-scoped** — Swatches, Styles, Themes, Documents. Same
  libraries, same gallery, regardless of which document is open.
- **Machine-scoped** — Library. Shared across every project on this
  computer.

If you save a project and open it on another machine, the document,
swatch, style, and theme data travels with it. The library doesn't —
anything you placed from the library was inlined into the document
SVG at place time.

## Where to next

Each section in this chapter walks one panel in detail:

- **Layers** (6.2) — the active document's structure, with hotkeys.
- **Library** (6.3) — system and user reusable artwork.
- **Swatches** (6.4) — named colours and palettes.
- **Styles** (6.5) — named fill / stroke / dash bundles.
- **Documents** (6.6) — the project's document gallery.
- **Themes** (6.7) — saveable doc-level surface presets.

For per-icon appearance editing (the inspector's Styling section,
which interacts with Swatches and Styles), see **Styling**
(5.4.5). For the in-inspector edit surface for the active document's
theme bundle (Canvas / Margins / Grid), see the **Theme** disclosure
(5.3.8).
