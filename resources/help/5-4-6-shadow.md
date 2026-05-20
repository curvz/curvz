# Shadow

The **Shadow** section adds a drop shadow to a selection — a
soft-edged offset duplicate beneath the artwork. It's the bottom
section of the **Object** group when present, sitting below
Styling per the convention that effects layer on top of fill
and stroke.

> **Appears when:** the primary selection is a node type that
> supports shadows. That covers most things you would want to
> shadow — paths, compound paths, groups, text, images, clip
> groups, blends, and warps. It does **not** cover layers, guides,
> reference points, measurement marks, grid layers, or margin
> layers — none of those have a render pass to shadow.

![The Shadow section showing Enable, OFFSET dx/dy, BLUR, COLOUR with alpha, and OPACITY](img/5-4-6-shadow-section.png)

## ❶ Enable shadow

A single checkbox at the top of the section. When **off**, the
rest of the controls go inert (greyed) but stay visible at their
configured values — flipping the toggle off and on again preserves
your existing shadow setup. The shadow only renders when this
checkbox is on.

The default shadow when first enabled is a sensible starting point
— small offset, modest blur, dark colour. Adjust from there.

## ❷ OFFSET — dx, dy

Two spin buttons under the **OFFSET** label:

- **dx** — horizontal offset. Positive values push the shadow
  rightward; negative leftward.
- **dy** — vertical offset. Curvz uses a Y-down convention for
  shadow offsets specifically (matches CSS and SVG `feDropShadow`)
  — positive values push the shadow downward, negative upward.
  This differs from the document's overall Y-up coordinate
  convention; the shadow offset matches the standard "shadow falls
  down and right" expectation directly.

Both spinners respect the document's display unit, with each
spinner showing the unit label to its right. Common values are
small — a 32-pixel icon usually wants 1–2 px in each axis at most.
Larger offsets read as a styled effect rather than a shadow.

## ❸ BLUR

A single spin button — Gaussian blur radius (standard deviation)
in document units. **0** is no blur (a hard-edged offset
duplicate); higher values soften the shadow's edge.

The blur radius scales with output size, so set it relative to
your icon's render dimensions. A 0.5 px blur reads cleanly on a
16-pixel icon; the same setting on a 256-pixel canvas barely
shows. Match blur to icon size, not to the canvas's pixel budget.

## ❹ COLOUR

A swatch and an alpha slider. Click the swatch to open the colour
picker (3.7). The picker shows the colour at full alpha; the
**slider next to the swatch** controls the colour's alpha
independently — you can dim a colour without re-picking it.

The swatch shows a checkerboard backdrop when the alpha is below
1.0, the same convention used elsewhere in the inspector for
translucent paints.

A typical shadow colour is black at 30–60% alpha, but for
coloured shadows (matching the icon's hue at low saturation, or
casting a coloured tint) the picker accepts any value.

## ❺ OPACITY

A second slider at the bottom of the section, from 0 to 100%.
This is a *post-effect* dimmer — it multiplies the shadow's final
alpha after the colour-alpha multiplication. Both controls feed
the renderer:

- **COLOUR alpha** is the colour's intrinsic translucency. Bake
  it in if you want the shadow to read as a tinted glow regardless
  of opacity.
- **OPACITY** is the dimmer for tweaking the shadow's overall
  presence without changing its tint.

Two sliders feels redundant on first read but matches the
Affinity/Illustrator convention — alpha-on-the-colour for tint
choices, opacity for taste-driven dimming. In practice you can
ignore one of them; pick whichever fits your mental model and
leave the other at full.

## Shadow versus blur

A drop shadow with offset 0/0 and a non-zero blur produces a
**glow** effect — symmetrical softening around the artwork. This
is a legitimate use of the section; Curvz does not have a separate
"glow" effect.

For shadows that should track the artwork's silhouette (an inner
shadow, a long shadow, etc.) the section does not cover those.
Inner shadows in particular require a different render approach
that Curvz does not support today.

## Performance note

Shadows on large blurry shapes can be heavy to render at high
canvas sizes. If the canvas feels sluggish during pan and zoom,
check whether large blurs are stacking (multiple shadowed objects
in front of each other). Drop the blur radius or temporarily
disable individual shadows to confirm.

For final export, the shadow rasterises into the SVG via standard
`feDropShadow` filters — the resulting file size impact is small,
but the render cost on the display side scales with the blur
radius and the shadowed object's bounding box.

## Where to next

- **Styling** (5.4.5) — fill, stroke, and overall opacity. The
  section directly above Shadow.
- **Color picker & paint editor** (3.7) — the picker that opens
  from the COLOUR swatch.
- **currentColor & symbolic icons** (9.2) — for icons that need
  to inherit colour from the host page, the shadow's colour can
  remain literal even when fill is currentColor.
