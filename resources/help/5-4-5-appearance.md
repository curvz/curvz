# Styling

The **Styling** section is the inspector's catch-all for how a
selection is painted — fill, stroke, opacity, and the bindings that
link those values back to the project's swatches and styles. Every
node that renders has a Styling section: paths, compound paths,
groups, clip groups, blends, text, images, and shadows all share
this single panel.

> **Section name.** This section is called **Styling** in the
> inspector. The header was renamed from *Appearance* in s148 m2
> so the application-tier **Appearance** section (the Dark/Light
> theme switch at the top of the inspector) could carry that
> label without collision. The file is still numbered 5.4.5 with
> the filename `5-4-5-appearance.md` for stable bookmarking;
> other manual chapters that reference "(5.4.5)" still point
> here.

The section reorganises around your selection. With one object
selected, it edits that object. With multiple, it broadcasts edits
to every sibling in the selection so a single click recolours the
whole set.

![The Styling section showing binding rows, Fill paint editor, Stroke paint editor with thickness and cap/join, and the section Opacity slider](img/5-4-5-appearance-section.png)

## Binding indicators

The very top of the section is a conditional block of up to three
**binding indicator rows**, each appearing only when the selection
has the corresponding binding. They are the single place to glance
to know whether the selection's appearance is *self-managed* or
*linked* to the project's named asset library.

- **Bound to style** — the selection inherits a full appearance
  preset (fill + stroke + effects) from a named style. Edits made
  here detach this binding by default.
- **Fill swatch** — the fill colour is linked to a named swatch.
  Repainting the swatch (in the Swatches panel, 6.4) updates every
  bound object live.
- **Stroke swatch** — same as fill swatch, for the stroke slot.

Each bound row has an **× Unbind** button that drops the binding
without changing the object's current paint values. Useful when
you want a one-off recolour that no longer follows the swatch.

When nothing in the selection is bound, the entire indicator block
collapses to zero height — the Fill and Stroke editors below
convey "unbound" implicitly.

For mixed selections (some bound to id A, some to id B, some
unbound), the row appears once with the value `<multiple>` and the
Unbind action drops every binding in the selection.

## ❶ Fill

A **Fill:** label and the project's standard **Paint editor**
widget below it. The editor packs three things into one row:

- A **type-toggle** with five options — **Solid**, **None**,
  **currentColor**, **Swatch**, **Gradient** — picking what kind
  of paint the slot carries.
- A **colour well** showing the current paint, clickable to open
  the colour picker (3.7).
- A **value strip** displaying the colour's name (region name like
  `Red` for unbound solids, swatch name for bound), or the
  gradient type for gradient paints, or `currentColor` for that
  special.

When the type is **Gradient**, an extra **Edit** affordance opens
the dedicated gradient editor where you can set stops, direction,
and falloff.

### currentColor

The **currentColor** type is a Curvz speciality — it tells the
exported SVG to inherit colour from the surrounding context (the
calling page's CSS, the icon-theme system, etc). For monochrome
icons that need to follow text colour automatically, set fill to
currentColor and you're done. See **currentColor & symbolic icons**
(9.2) for the deeper story.

## ❷ Stroke

Below Fill is the **Stroke:** label and another paint editor with
the same five-option type-toggle. Below the editor are
stroke-specific properties that have no fill equivalent:

- **Thickness** — the stroke width in document units. Respects the
  display unit. Edit live; commits coalesce on undo.
- **Cap** — three icon toggles: **butt** (flat), **round**
  (semicircular), **square** (extends past the endpoint by half
  the thickness). One is always active.
- **Join** — three icon toggles: **miter** (sharp corner),
  **round**, **bevel** (chamfered). Applies where two stroke
  segments meet at an angle.

The Cap and Join icons match the standard SVG stroke conventions —
if you have used Inkscape or Illustrator, the visual symbols read
the same way.

When the stroke type is **None**, the Thickness / Cap / Join rows
go inert — there's no stroke to set widths on. They become live
again the moment you flip stroke to any other type.

## ❸ Opacity

A separator and a single slider at the bottom of the section, from
0 to 100%. Applies to the entire object — fill, stroke, and shadow
all dim together. This is a rendering-time multiplier, not a
recolour: the underlying fill and stroke colours stay where they
were, the object just renders with reduced alpha.

For per-paint alpha (a translucent fill while the stroke stays
opaque), use the colour picker's alpha control on each paint
individually. Use Opacity here for whole-object dimming only.

## Multi-object selection

With more than one object selected, every change you make in the
Styling section is **broadcast** to the entire selection. Picking a
new fill colour repaints them all; flipping cap from butt to round
applies the new cap to every one. The broadcast is a single
composite undo step — Ctrl+Z reverts the whole batch at once.

Bindings break for siblings as part of a broadcast. If three of
five selected objects were bound to the swatch *Brand Red* and you
recolour the selection in the Styling editor, all five end up with
the literal new colour and no swatch binding. To keep bindings,
use the Swatches panel (6.4) to recolour the swatch itself, not
the objects.

## Style binding versus swatch binding

The two binding kinds carry different scope:

- **Swatch binding** (per-paint) tracks one colour. The fill slot
  and stroke slot bind independently — fill can be swatch-bound
  while stroke is unbound, or vice versa.
- **Style binding** (per-object) tracks a complete appearance —
  fill, stroke, all effects together as one named preset. When
  you bind to a style, the per-paint swatch bindings are
  superseded; the style is the single source of truth for that
  object.

See **Swatches** (6.4) and **Styles** (6.5) for the Content-side
panels that manage these.

## Where to next

- **Color picker & paint editor** (3.7) — the underlying picker
  used by the colour wells in this section.
- **Shadow** (5.4.6) — drop shadow, the section directly below
  Styling for any node that supports one.
- **Swatches** (6.4) and **Styles** (6.5) — managing the named
  paint and appearance presets the binding rows track.
- **currentColor & symbolic icons** (9.2) — when and why to use
  the currentColor paint type.
