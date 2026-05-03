# Library

The **Library** panel is a gallery of reusable SVG artwork. Drop a
file in the right place on disk and it appears here, organised by
category. Click any thumbnail to drop the artwork into the active
document at the centre of the canvas.

The library is shared across every Curvz project on the machine —
it isn't part of any individual project's data. Anything you place
from the library is *inlined* into the document at place time, so
the project file remains self-contained.

![The Library panel with three categories](img/6-3-library-thumbs.png)

## ❶ Two scan locations

Curvz scans two folders on startup ❶:

- **System library** — `/usr/share/curvz/library/`. Read-only.
  Items shipped with the Curvz install live here; on most systems
  it is empty unless the package included sample artwork.
- **User library** — `~/.config/curvz/library/`. Yours to fill.
  Items dropped here are picked up the next time the panel
  refreshes.

Inside each location the **immediate sub-folders become categories**:
`~/.config/curvz/library/Symbols/heart.svg` shows up under a
*Symbols* heading. SVG files placed directly at the top level (with
no parent folder) are skipped — Curvz needs at least one folder
level for the category name. If a category name appears in both
system and user locations, items from both are merged into the same
heading.

## ❷ Adding to the library

The header row has an **Add to Library** button ❷. It opens a small
form that asks for a category and copies the currently-selected
object on canvas into the user library as a new SVG. If you don't
have a selection, the button is disabled.

You can also bypass the panel and drop SVGs into the user library
directory directly from your file manager. The panel doesn't watch
the folder live — use the **refresh** button next to the add button
to re-scan after copying files in by hand.

## ❸ Category sections

Each category renders as ❸ a collapsible section with a colour-coded
header. Click the arrow to fold or unfold it. Inside the section is
a flow-grid of thumbnails: each one a card showing a small Cairo
render of the SVG and the file's name underneath (the name is
the filename minus the `.svg` extension).

The thumbnail respects the project's current motif — artboard
colour, workspace colour — so the preview matches the surface the
icon will sit on once placed.

## ❹ Placing an item

Click any thumbnail to **place** ❹ the item. The SVG content is
parsed and inserted into the active document on the active layer,
positioned at the centre of the artboard. The placed object is
selected immediately so you can move it, resize it, or restyle it
right away.

The library doesn't keep a live link back to the source file. Once
placed, the artwork is part of the document — editing the original
SVG on disk doesn't change documents that have already imported it,
and conversely, edits inside Curvz don't write back to the library.
If you want to update artwork everywhere, replace the file in the
user library and re-place from each document where needed.

## What lives well in the library

The library works best for artwork you want to drop in cleanly and
modify in place: arrowheads, glyph shapes, repeated motifs, common
constructions you've worked out once and want to reuse. It's less
useful for full icons — those generally live in the project's
**Documents** panel (6.6) instead, where each is a first-class
sibling icon rather than a copy-pasted block.

Two practical conventions:

- Keep filenames short and descriptive. They become the visible
  card label.
- Group related items into categories with names you'll recognise
  six months later. The folder structure is the only organisation
  the panel offers.

## Where to next

- For named *colours* shared across the project, see **Swatches**
  (6.4).
- For named *appearance bundles* (fill, stroke, dash) shared across
  the project, see **Styles** (6.5).
- The Documents panel that holds full sibling icons is covered in
  **Documents** (6.6).

If you want the library to refresh automatically as you add files,
that's not currently wired — use the refresh button in the panel
header after dropping new SVGs in.
