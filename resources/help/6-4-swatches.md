# Swatches

The **Swatches** panel is your project's named colour library.
Colours you save here become reusable: click a swatch chip to apply
it to the current selection's fill, Alt-click to apply to its
stroke. Swatches are organised into palettes — bundles of related
colours — and the panel keeps a recents strip across the top so
you can pick up colours you've used recently without hunting.

Swatches save with the project. A `.gpl` interchange format lets
you share palettes between projects or with other tools (Inkscape,
GIMP, Krita and others use the same format).

![The Swatches panel with a palette and recents strip](img/6-4-swatches-grid.png)

## ❶ Header — title, search, and the kebab menu

The header row ❶ has the panel title, a search field, and a single
kebab (⋮) button that holds every operation:

- **New swatch…** — opens the colour picker; pick a colour and
  Curvz adds a new swatch to the active palette. The picker stays
  open so you can keep adding without re-opening the menu.
- **New palette…** — prompts for a name and creates an empty
  palette.
- **Load palette ▶** — submenu listing bundled palettes (shipped
  with Curvz) and any user palettes you've previously exported.
- **Import…** — pick a `.gpl` file from disk. The palette is added
  to the project under its file name.
- **Export current…** — write the active palette out as a `.gpl`
  file. Defaults to `~/.config/curvz/palettes/`, after which it
  shows up in the Load submenu automatically.
- **Rename / Duplicate / Delete** the active palette.

The first colour you save creates a "Default" palette automatically
if none exists.

## ❷ Search

The search field ❷ filters the visible chips. The match is
case-insensitive and runs against three things at once:

- the swatch's **name**,
- its **hex value** (e.g. `ff8000`),
- its **r,g,b** triple as plain decimals.

If you remember the colour was called *brand orange* but not where
it is, search for `brand`. If you remember the hex but not the
name, type the hex. Search applies to whatever you're currently
seeing — recents and the active palette grid filter together.

## ❸ Palette chooser

The dropdown ❸ at the top of the body switches the visible palette.
The kebab acts on whichever palette is selected here, and new
swatches go into this palette. Switching palettes is a panel-only
operation — it doesn't move any swatches.

## ❹ Recents strip

The horizontal **Recents** strip ❹ lists the swatches you've used
most recently across the whole project, in MRU order, regardless
of which palette they belong to. Apply any swatch (from any panel)
and it bubbles to the front of recents. The strip is empty in a
fresh project.

## ❺ Chip grid

Below recents, the chip grid ❺ shows the active palette's swatches
in a wrapping flow layout. Each chip is a 24-pixel square drawn
with the swatch's colour. Layout is single-row when the panel is
narrow and grows into multiple rows as the panel widens.

A chip you click applies its colour as a **fill** to the selection.
**Alt-click** applies it as a **stroke** instead. This matches the
Eyedropper convention.

A chip whose colour matches the current selection's fill is drawn
with an **active-paint ring** around it — a thin white-on-black
halo. Multiple chips can light up if several swatches share the
same colour. The ring updates live as you change selection.

## ❻ Right-click on a chip

Right-clicking a chip opens a context menu:

- **Edit colour** — opens the picker pre-loaded with this swatch.
  Closing with a new colour updates the swatch in place; existing
  bindings to it (objects with their fill set to this swatch
  reference) re-paint with the new colour automatically.
- **Rename** — type a new display name.
- **Duplicate** — copies the swatch into the same palette under a
  new id.
- **Delete** — removes the swatch from the project. Recents is
  cleaned up alongside.
- **Move to palette ▶** — submenu listing every other palette;
  picking one moves the swatch out of its current palette into
  that one.

For chips appearing only in the recents strip (no palette), the
move submenu reads **Add to palette** instead of Move to.

## ❼ Esc-to-cancel during creation

When you open the picker via **New swatch…**, the swatch is created
the first time you change a colour value (so live-preview works
against an actual project entity). If you press **Esc** before
committing, Curvz removes the swatch you were creating — Esc means
"never mind", not "keep what's there". For *editing* an existing
swatch, Esc reverts to the original colour without deleting
anything.

## SwatchRefs and live recolour

Inside the document, fill and stroke can either be a literal colour
or a **swatch reference** — a small id that points to a swatch
entry in the library. When you click a chip to fill the selection,
the object stores the reference, not the colour itself. Editing the
swatch later automatically updates every object that references it
without having to re-apply.

The inspector's **Appearance** section (5.4.5) shows whether a
fill or stroke is bound to a swatch and lets you unbind it back to
a literal colour.

## .gpl interchange

The bundled and user palettes use the same `.gpl` text format that
GIMP, Inkscape, Krita and others read. A `.gpl` file is a plain
list of `R G B Name` triples with a small header — readable in any
text editor, easy to hand-edit if needed.

Importing a `.gpl` adds a new palette to the project. Exporting
writes the *active* palette out so you can share it or carry it to
another project.

## Where to next

- For *combinations* of fill + stroke + dash, see **Styles** (6.5).
- For per-object appearance editing (which surfaces "Bound to
  swatch X" annotations), see **Appearance** (5.4.5).
- The colour picker itself, used by the New / Edit flows, is
  covered in **Color picker & paint editor** (3.7).
