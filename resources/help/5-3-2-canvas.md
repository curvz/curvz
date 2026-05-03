# Canvas

The **Canvas** section sets the size, orientation, and unit
system of the active document. It is the second section in the
inspector's **Document** group and the one you reach for whenever
the icon you are designing needs different dimensions, a different
working unit, or a quick portrait/landscape flip.

![The Canvas section in Pixel mode with W/H spinners and the orientation buttons](img/5-3-2-canvas-section.png)

## ❶ Units

The **Units** dropdown at the top of the section controls how
sizes and positions are *displayed* throughout the rest of the
inspector. It does not change the document's geometry — only how
that geometry reads to you. Four choices:

- **px** — pixels. Best for icon work and screen-bound output.
- **in** — inches. Best for print at known DPIs.
- **mm** — millimetres. Print-friendly and metric.
- **pt** — typographic points (72 to the inch).

Switching units rebuilds every other inspector field that shows a
distance, so you immediately see your existing geometry in the
new unit.

## ❷ Mode

The **Mode** dropdown controls how Curvz *thinks about* the
canvas size. Three modes, each with its own size widgets below:

- **Pixel** — the canvas has fixed pixel dimensions. Best for
  icons targeted at a specific render size.
- **Physical** — the canvas has physical dimensions (inches,
  millimetres, points) at a chosen DPI. Best for print, business
  cards, signage.
- **Ratio** — the canvas is a ratio plus a quality budget. Best
  when the same artwork needs to render at many sizes — Curvz
  scales the editing surface to a quality target without
  committing you to a specific output resolution.

Switching modes converts the current canvas into the new mode's
representation; nothing is lost as long as you do not also change
the size in the same step.

## ❸ Orientation

To the right of the Mode dropdown are two icon buttons —
**Portrait** and **Landscape**. Whichever matches the current
canvas aspect is highlighted. Tap the inactive one to swap width
and height. A square canvas counts as portrait by convention; the
landscape button on a square canvas swaps W and H even though the
result is still square (this keeps the orientation state clean).

## Mode-specific size widgets

The fields under the Mode row depend on which mode you picked.

### Pixel mode

A row of preset buttons — **16, 24, 32, 48, 64, 128, 256** —
gives a one-tap path to a square canvas at that pixel size. They
are the Linux icon-theme standard sizes plus a few common Web
sizes. Tap any preset to overwrite both width and height.

Below the presets are **Width (px)** and **Height (px)** spin
buttons. Type any value, or use the arrow keys. The valid range
is 1 to 65 536 in either dimension.

### Physical mode

Three rows: **Width**, **Height**, and **DPI**. Width and Height
are shown in the current display unit — change Units at the top
of the section and the spinners refresh in the new unit
(internally Curvz always stores physical sizes in inches, so the
conversion is lossless).

The **DPI** row has a dropdown of common print/screen DPIs — 72,
96, 150, 300, 600 — plus a spin button on the right that accepts
any value from 1 to 9 600. The dropdown is a quick-pick; the
spinner is authoritative. Picking a preset writes the spinner;
typing a custom value into the spinner switches the dropdown to
**Custom**. Use 96 for screen, 300 for general print, 600 for
fine print. Outliers like 144 (retina), 203 (thermal printer),
and 1 200 (high-end print) are valid.

### Ratio mode

Six **preset buttons** along the top — common aspect ratios like
1:1, 3:2, 4:3, 16:9, A4, US Letter. Tap a preset to set the
ratio.

Below them are **Ratio W** and **Ratio H** spin buttons accepting
0.001 to 100 with three decimal places of precision. Curvz
normalises whatever you type so the shorter axis is 1.0 — type
1920 × 1080 and the field will redisplay as roughly 1.778 × 1.0.

The **Quality** row sets the unit count along the short axis. It
governs how detailed Curvz makes the editing surface. The slider
labels — **Icon, Print, Poster, Billboard** — are calibration
hints:

- **Icon**: ~1 000 — small, on-screen artwork.
- **Print**: ~4 000 — A4-style pages.
- **Poster**: ~10 000 — wall art.
- **Billboard**: 50 000+ — huge format.

Below the spinner is a read-only **Pixels** row that translates
the current ratio + quality into the equivalent pixel dimensions.
Use it as a sanity check when you are not yet sure what quality
budget you need.

## Mode trade-offs

Pick the mode that matches how the icon will be used:

- **Pixel** is the right answer for almost all icon work. The
  output target is exact, the units are unambiguous, and every
  decision is in the same currency.
- **Physical** is right when the artwork has a real-world print
  size — a sign, a card, a label. DPI choice locks how much
  detail will survive printing.
- **Ratio** is right when you do not yet know the output size or
  when one source needs to render correctly at many sizes. The
  quality knob lets you trade editing surface area for finer
  pixel-equivalent precision later.

You can change modes at any time. Curvz preserves the canvas's
absolute pixel size when switching, recomputing the other
representations to match.

## Where to next

The Canvas section is the most numeric of the Document-group
sections. The four that follow — **Guides, Grid, Margins, Snap**
— are the layout helpers Curvz draws *on top of* the canvas. None
of them export with the SVG; they exist to make precise layout
easier while you draw.

If you are looking to change *which* unit the rulers display,
remember that Units here is a Document-group setting and applies
to the whole document. Per-tool unit overrides do not exist —
this dropdown is the single source of truth.
