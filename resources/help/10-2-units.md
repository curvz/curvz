# Units

Curvz lets you work in your unit of choice. The internal storage
is always pixels (96 px to the inch, the SVG / CSS standard), but
every numeric field in the inspector and every ruler tick can
display in **px**, **in**, **mm**, or **pt** — whichever you
prefer. The unit is per-document, so you can have two icons in
the same project, one in millimetres for a print-derived asset
and one in pixels for a screen icon.

The unit setting is purely cosmetic — it changes how numbers
read in the UI without ever changing the geometry. Switching
from mm to px doesn't reshape your icon; it just shows the same
positions in different units.

![The unit dropdown in the Canvas inspector section](img/10-2-unit-dropdown.png)

## ❶ Where the setting lives

The active unit ❶ is set in the inspector's **Canvas** section
(5.3.2) — there's a small dropdown labelled **Unit** with the
four options. Changing it updates the rulers, every spinner in
the inspector, and every readout in the status bar instantly.

The unit lives on the document, so it travels with the document
through save and load. A project can include documents in
different units side-by-side — switching tabs flips the visible
unit accordingly.

## ❷ The four units

The conversion factors are exact, all relative to 96 px / in:

- **px** (pixels) — the SVG / CSS native unit. 1 px is 1 SVG
  user unit. Default for new documents.
- **in** (inches) — 1 in = 96 px exactly.
- **mm** (millimetres) — 1 mm ≈ 3.78 px (96 ÷ 25.4).
- **pt** (points) — 1 pt ≈ 1.33 px (96 ÷ 72).

Choose by what your inputs feel most natural in. Screen icons
are usually pixels. Print artwork is usually millimetres or
inches. Typography work tends to be points.

## ❸ Expressions in spinners

Most numeric spinners — inspector position and dimension
fields, toolbar fields, dialog fields, transform rows in
the Selection tool's context bar — accept **expressions**,
not just plain numbers. (A handful of older spinners, like
the Scale and Skew rows and the New Document dialog's
dimensions, are still plain-number-only; they're slated for
the same upgrade.) Type something like

```
4.25in + 16mm * 2
```

into a position field, press Enter, and Curvz computes the
result in pixels and stores that. The unit suffixes (`in`,
`mm`, `pt`, `px`) work in any combination; bare numbers are
interpreted in the field's *default* unit (whatever the active
display unit is for the document).

The grammar supports addition, subtraction, multiplication, and
division with standard precedence. Parentheses group. White-
space is ignored.

A few patterns this makes easy:

- **Snap to a fraction.** `5mm / 3` for a precise three-way
  division — you don't have to compute the decimal yourself.
- **Add a margin.** With the cursor in an X spinner reading
  `12mm`, type `12mm + 4mm` to push by exactly 4 mm without
  reaching for a calculator.
- **Mix units in one expression.** `1in + 8mm` works. The
  result is converted to pixels internally.

Invalid expressions don't take effect — the spinner reverts to
its prior value and the status bar reports a parse error.
That's also true if a unit suffix is rejected by the field's
context: an angle field accepts plain numbers but refuses unit
suffixes, since "30 mm" doesn't mean anything for an angle.

## ❹ Round-trips and precision

Internally every coordinate is double-precision pixel-space.
Switching units only changes how that pixel value is *displayed*
— the underlying value is unchanged. So:

- A path placed at `10mm` in mm units shows as `37.795...px`
  if you flip to px. Same geometry, different label.
- Saving and reloading a document preserves its unit setting
  — the document remembers it was last edited in mm.
- The SVG export always emits raw pixel values; the unit
  setting is editor-state, not part of the icon data.

Round-trip precision is high enough that switching units back
and forth doesn't accumulate error. There's no in→mm→in drift.

## ❺ Rulers

The rulers along the top and left edge of the canvas use the
same display unit. Tick spacing scales with zoom — Curvz picks a
sensible interval automatically. Switch units and the rulers
relabel; they don't redraw the canvas, just the tick text.

The ruler origin is independently set with Alt-click on the
corner square (see **Canvas, rulers & corner**, 3.6). Origin and
unit are different things — the unit changes labels, the origin
changes where labels start counting.

## Where to next

- For the **Canvas** inspector section that hosts the unit
  dropdown alongside artboard size, DPI, and orientation, see
  5.3.2.
- For rulers, ruler origin, and canvas anatomy, see **Canvas,
  rulers & corner** (3.6).
- For zoom presets that interact with how units feel at
  different magnifications, see **View options** (10.1).
- For per-document settings that pair well with units (units
  is one of the things a Theme bundles), see **Themes** (9.1).
