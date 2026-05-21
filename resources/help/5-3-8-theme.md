# Theme

The **Theme** disclosure groups the three doc-level surface
settings that travel together as a saveable style preset:
**Canvas** (the artboard / workspace / creation colours),
**Margins** (5.3.5), and **Grid** (5.3.4). Click the Theme
header to fold or unfold all three at once.

The Themes content panel (see 9.1) saves and reapplies the
theme bundle — pick a theme there and Curvz writes the saved
values back into the active document. The bundle covers more
ground than this disclosure shows: alongside Canvas, Margins,
and Grid it also round-trips the document's display unit
(edited in Dimensions, 5.3.2), the guides colour (edited in
Object ▸ Guides, 5.3.3), and every snap rule (edited from the
toolbar Snap switch's right-click popover). The full inventory
is in the bundle section below; see 9.1 for the apply mechanics.

> **Section location:** The Theme disclosure is numbered 5.3.8
> in this manual to keep the existing 5.3.1–5.3.7 numbers stable.
> In the inspector itself the disclosure sits *between* Metadata
> (5.3.1) and Dimensions (5.3.2), wrapping Canvas, Margins, and
> Grid in that build order.

![The Document group with the Theme disclosure expanded](img/5-3-8-theme-disclosure.png)

## What's in the bundle

The three sections inside the disclosure each have their own
help page, but here's what travels together when you save or
apply a theme:

- **Canvas** colours — artboard, workspace, and creation, each
  with separate values per app appearance mode (Dark / Light).
  Documented below in this page.
- **Margins** (5.3.5) — enable state, four insets, column and
  row counts and gaps, and the overlay colour.
- **Grid** (5.3.4) — enable state, style (lines vs. dots),
  spacing X/Y, offset X/Y, and the overlay colour.

Three more pieces ride along in the bundle even though their
edit surface lives outside this disclosure:

- The document's **display unit** (px / in / mm / pt), edited in
  **Dimensions** (5.3.2).
- The **Guides** colour, edited in **Object ▸ Guides** (5.3.3) —
  moved out of the Theme disclosure in s179 m3 but still saved
  here.
- Every **snap rule** — snap to artboard / grid / guides /
  margins / objects / intersections / refpoints, plus the snap
  distance — edited from the toolbar's Snap switch right-click
  popover (Snap left the inspector in s150).

The disclosure holds the three sections with on-disclosure edit
surfaces; the bundle adds these three riders. The Themes
content panel (9.1) lists the full inventory.

The Dark/Light **theme switch** is *not* in this disclosure —
that's app-tier, in **Application ▸ Appearance** at the top of
the inspector. The Theme disclosure here is about the
*per-document* style bundle, not the app's own chrome.

## The Canvas section

Inside the Theme disclosure, the **Canvas** section is the home
for the three doc-level surface colours. They are editor-only —
the canvas colours never appear in the exported SVG.

> Don't confuse this **Canvas** section with the **Dimensions**
> section (5.3.2). Until s148 the geometry section was called
> *Canvas* and these colours were called *Motif*. The rename
> swapped the names: geometry became Dimensions, surface colours
> became Canvas. The function names in the source still carry
> their older identities (`build_canvas_section` is the
> Dimensions builder; `build_canvas_colours_section` is the one
> documented here).

![The Canvas section inside the Theme disclosure](img/5-3-8-canvas-colours.png)

### Three colour rows

Three labelled colour swatches stack inside the section. Click
any swatch to open the colour picker — see **Color picker &
paint editor** (3.7) for the picker itself.

- **Artboard** — the colour of the rectangle the icon sits on
  (the printable extent of the document).
- **Workspace** — the colour of the area surrounding the
  artboard (the pasteboard region where you can park
  work-in-progress outside the export bounds).
- **Creation** — the colour Curvz uses while you are *making*
  something but have not yet committed it. Drag a rectangle and
  the live preview draws in this colour. Plot points with the
  line tool and the in-progress segments are this colour. Build
  a Bézier curve with the pen tool and the path being traced is
  this colour, along with its node handles. Once you release
  and the shape commits, it adopts whatever fill and stroke its
  appearance settings specify.

Selection-time visuals (the dashed rectangle around a selected
object, resize handles, the marquee you drag to select multiple
objects) are a separate concept and are not affected by Creation.

### Per-appearance-mode colour memory

Curvz stores **separate Artboard, Workspace, and Creation
colours for each app appearance mode**. If you set a particular
trio while in Dark mode, then flip to Light (in Application ▸
Appearance) and pick different colours there, both customisations
are remembered. Flipping back to Dark restores the dark-mode
trio you chose; flipping to Light restores the light-mode trio.

This matters because the colour that reads well depends on the
surrounding chrome. A medium grey that lifts an icon nicely on
a dark panel can feel sunken on a light one. A bright cyan that
pops against a dark artboard turns into glare against a white
one. Letting each mode keep its own trio means you tune both
surfaces once and never have to re-pick when toggling.

The Curvz defaults are deliberately neutral greys for artboard
and workspace in both modes — Curvz is not a "paper" editor,
so Light mode does not assume white. The Creation defaults flip
luminance between modes (light blue in Dark mode, deep blue in
Light mode) so the construction preview always has clear
contrast against the artboard.

### Reset button

The **Reset** button below the three swatches restores the
*current appearance mode's* Artboard / Workspace / Creation
trio to Curvz's factory defaults. It does not touch the other
mode's trio, and it does not flip the appearance mode itself.

Use Reset when you have over-tuned one mode and want a clean
baseline back without disturbing your work in the other mode.

## Where to next

- **Dimensions** (5.3.2) — the doc's geometric bones (size,
  units, ratio/quality). Sits below the Theme disclosure in the
  Document group.
- **Margins** (5.3.5) and **Grid** (5.3.4) — the two layout
  overlays inside the Theme disclosure.
- **Themes** content panel (9.1) — saving and reapplying full
  theme bundles across documents.
- **Application ▸ Appearance** — the Dark/Light app appearance
  switch (a separate, app-tier setting). Each per-doc Canvas
  trio is bound to one appearance mode, so flipping the app
  switch repaints the canvas with that doc's other trio.
