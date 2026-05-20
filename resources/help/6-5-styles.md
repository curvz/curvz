# Styles

A **Style** is a bundled appearance: fill, stroke, stroke width,
dash pattern, and any related per-property settings, saved as a
single named entry. The **Styles** panel lists every style available
to the project — built-in styles shipped with Curvz, plus any you
create yourself — and lets you bind a selection to a style with one
click.

The point of styles is consistency. Build a style once, bind every
icon's primary line to it, and tweaking the style updates every
bound icon at once. They sit one level above swatches: where a
swatch is one colour, a style is the whole appearance recipe.

![The Styles panel with the Built-in category active](img/6-5-styles-list.png)

## ❶ Header — category dropdown and the kebab menu

The header row ❶ has the **category chooser** and a single kebab
(⋮) button. The chooser switches which category's chips are
visible — only one category shows at a time, mirroring how the
swatch panel handles palettes.

The kebab menu holds every operation:

- **New style…** — opens the Style Editor seeded with sensible
  defaults. OK adds a new entry to the active user category.
- **New style from selection** — reads the primary selected
  object's appearance (fill, stroke, width, dash) and creates a
  style around it. Auto-named after the object if it has a name,
  otherwise *Style N* with the lowest free number.
- **New category…** — prompts for a category name and creates an
  empty style under it. Categories are birthed by birthing a
  member; an empty category would disappear on next reload, so the
  flow asks for both at once.
- **Import…** — load a `.json` style pack from disk into the user
  tier. Names that clash get a numeric disambiguator.
- **Export…** — write every user-tier style out as a `.json` pack
  for sharing or backup.
- **Load user pack ▶** — submenu listing previously-exported packs
  in `~/.config/curvz/styles/`.

When the active category is read-only (built-in / app-tier), the
edit and rename items disappear from the kebab.

## ❷ Built-in vs user tier

Styles split into two tiers:

- **Built-in (app-tier)** styles ship with Curvz. They live under
  the **Built-in** category and cannot be renamed, edited, or
  deleted. You can right-click a built-in style and choose
  **Edit a copy…** to duplicate it into your user tier with a
  *copy* suffix on the name, then customise freely.
- **User-tier** styles are everything you create. Categories you
  add show up in the chooser alongside the Built-in entry.

The category chooser distinguishes the two — the Built-in entry is
unique to the app tier; user categories carry their own names.

## ❸ Chip grid

Each style is a chip ❸ — a 24-pixel preview that captures the
appearance:

- The **inner area** renders the fill colour. Diagonal stripes if
  the fill is *None*; a small "C" glyph if the fill is
  `currentColor`.
- The **outer ring** is a constant-thickness band rendering the
  stroke colour. Independent of `stroke.width` — the chip is a
  colour signal, not a thickness preview. No ring at all if the
  stroke is *None*.
- For paints bound to a swatch (a SwatchRef), the chip resolves
  through the swatch library to the swatch's *current* colour, so
  editing a swatch elsewhere updates every chip that references
  it.
- A chip whose style is bound to the current selection draws an
  **active-paint ring** overlay (white-on-black) on top of itself.

Hover any chip and a multi-line tooltip appears with the full
recipe — name, tier, fill source, stroke source, width, dash
description.

## ❹ Binding the selection

**Double-click** a chip ❹ to bind every Path or Compound in your
current selection to that style. Each object's fill, stroke, width,
and dash are overwritten with the style's values, and the
object remembers the binding. The binding is recorded as one atomic
undo step — Ctrl+Z reverts every affected object together.

Once an object is bound, editing the style later updates that
object automatically. To break the binding (without changing the
appearance), use the **Style** row at the top of the inspector's
Styling section (5.4.5), or press `Ctrl + Shift + B` to unbind
the selection from any style it currently carries.

## ❺ Right-click on a chip

The right-click menu varies by tier. On a **user-tier** chip:

- **Edit…** — opens the Style Editor with the existing values.
  OK pushes one undo entry that updates the style; every bound
  object's appearance refreshes.
- **Rename** — text prompt for a new name.
- **Duplicate** — copies the style in place.
- **Set category…** — moves the style to a different category, or
  to *(uncategorised)*.
- **Delete** — removes the style. Bound objects keep their
  appearance — the binding is dropped, so they become unbound but
  visually unchanged. Undo restores both the style and every
  binding.

On a **built-in (app-tier)** chip:

- **Duplicate to user** — copies the built-in style into your user
  tier under a fresh id.
- **Edit a copy…** — the duplicate-and-open-editor convenience
  flow.

Built-in styles never expose Edit, Rename, Set category, or Delete.

## ❻ Categories

Categories are user-defined groups inside the user tier. Every
user-tier style belongs to one category, or to *(uncategorised)*.
The chooser cycles through them; the kebab includes
**Rename category…** and **Delete category…** items when a user
category is active. Deleting a category removes every style in it
(atomic via undo — one Ctrl+Z restores the whole bunch).

The **Built-in** category is always present and always read-only.
You can't rename it, delete it, or move user styles into it.

## JSON interchange

Style packs use a small project-defined JSON format (not a
GIMP-derived format like swatches). Export writes every user-tier
style to one file; import reads packs back. The bundled-pack
submenu lets you load curated starter packs, and exporting to
`~/.config/curvz/styles/` makes the pack appear in the *Load user
pack* submenu automatically.

App-tier styles never export — they're code, not user data.

## Where to next

- The **Styling** section (5.4.5) inside the inspector is where
  bindings show up per-object, and where you can unbind without a
  shortcut.
- The colour picker used by the Style Editor's fill and stroke
  rows is covered in **Color picker & paint editor** (3.7).
- For Curvz's other named-preset system — the doc-level **Theme
  bundle** that packages Canvas colours, Margins, Grid, Guides,
  units, and snap settings as one applyable unit — see the
  **Themes** panel (6.7) and the conceptual chapter **Themes**
  (9.1).
