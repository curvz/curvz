# Text

The **Text** section *replaces* the standard Selection section
(5.4.1) when the selected object is a text node. Where Selection
shows generic POS / SIZE / transform rows, Text shows the controls
that actually matter for type — content, font, size, alignment,
baseline, letter spacing, and optional linking to a path.

> **Appears when:** a text node is the primary selection. Created
> with the **Text** tool (T) or **Text on Path** tool (U). For any
> other selection — paths, groups, images, refs — an empty
> "Text" placeholder collapsible appears below Selection in the
> Object group, but its body is empty since none of these
> properties apply.

> **Section position:** The Text section is a sibling of Selection
> inside the Object group, not nested inside it. For text nodes,
> the Selection section is not built at all and Text takes its
> place; for non-text nodes, both headers appear (Selection
> populated, Text empty).

![The Text section showing content, font, size, alignment, and on-path controls](img/5-4-2-text-section.png)

## ❶ Text

The first row is the **content** entry — the actual string the text
node renders. Type into it and the canvas updates live as you type;
no Enter required. Edits coalesce on undo, so a run of typing is one
Ctrl+Z step rather than per-character.

The entry is a single line. Multi-line text is not supported in the
inspector field today; press the canvas escape and use the Text
tool's on-canvas editor for multi-line content.

## ❷ POS

A pair of spin buttons — **X** and **Y** — for the text's anchor
position. The anchor's exact role depends on the **Align** setting
below:

- Left-aligned (⇤) — anchor sits at the left edge of the text run.
- Centre (≡) — anchor sits at the horizontal centre.
- Right (⇥) — anchor sits at the right edge.
- Justify (≣) — like left, with extra spacing distributed across
  the run.

Both spinners respect the document's display unit. Edits coalesce
on undo.

## ❸ Font

A searchable dropdown of every system font Pango can find. Type to
filter; pick to apply. The change is live — the canvas redraws on
selection.

The font list is alphabetised. There is no preview rendering of
each option in the dropdown today; pick by name and watch the
canvas.

## ❹ Size

A spinner from 1 to 2000 (in the document's display unit). Default
step is 1, page-step is 10. Live update on canvas.

## ❺ Bold / Italic

Two checkboxes. Either, both, or neither — the four combinations
are independent and applied via Pango. Effective only if the chosen
font has a Bold or Italic face; if it does not, Pango falls back to
synthetic emboldening or slanting (which can look poor at icon
sizes).

## ❻ Baseline

A single spin button — perpendicular offset of the text from its
baseline. Positive values lift the run upward (toward the canvas
top in Y-up); negative values drop it. Most useful when the text is
attached to a path (see below) — baseline shift moves the run
above or below the path stroke.

For free-floating text the baseline shift is just a vertical offset
relative to POS Y.

## ❼ Spacing

A single spin button — extra advance between glyphs. Zero is the
font's natural spacing. Positive values open the text out (wider);
negative values tighten it. Excessive negative spacing causes
glyphs to overlap.

This is a per-character extra advance, not Unicode-aware kerning —
it applies uniformly across the run regardless of which character
pair sits at any position.

## ❽ Align

Four toggle buttons — **Left** (⇤), **Centre** (≡), **Right** (⇥),
**Justify** (≣). Exactly one is active at a time. Each sets *both*
the SVG `text-anchor` (where the anchor point sits relative to the
run) and the Pango paragraph alignment.

For single-line text the four behave roughly like you'd expect.
Justify produces extra inter-word space to fill the line; for
single-word lines it behaves like Left.

## ❾ Path

The bottom block is the **text-on-path** controls. Visible behaviour
depends on whether the text is currently linked to a path:

### Not linked

- **Path** row shows `—`. No Detach button.

To attach the text to a path, switch to the **Text on Path** tool
(U) and click the target path. The Text section will refresh and
show the controls below.

### Linked

- **Path** row shows the linked path's id, with a **Detach**
  button. Detaching keeps the text in place at its current position
  along the path; the run becomes free-floating again.
- **Offset** spin — start position along the path (in document
  units). 0 is the path's first node; positive values move the
  start point along the path's direction.
- **Flip** checkbox — **Below path**. When off, the text sits on
  the path's natural side (above for clockwise paths). When on,
  the text mirrors to the opposite side. Useful for circular
  layouts where part of the run reads better on the inside.

The detach action and Below-path toggle both push undo as a single
step.

## What's not in the Text section

A few common text properties live elsewhere or do not exist in
Curvz today:

- **Fill and stroke** — the text's appearance lives in the
  **Styling** section (5.4.5), the same widget every other
  paintable node uses. Text honours both fill and stroke; for
  monochrome icons leave stroke at None.
- **Drop shadow** — the **Shadow** section (5.4.6) applies to text
  the same way it applies to paths.
- **Multi-line wrapping**, **vertical text**, **rich-text runs**
  with mixed fonts — not supported. Curvz is an icon editor; one
  text node per run, one font per run.

## Where to next

The companion topic is **Text on Path** (4.5.2) under Tools, which
covers the U-tool flow that produces a linked text node. The
**Text** tool (4.5.1) covers free-floating text creation.

For the inspector sections that paint and shadow text the same way
they paint paths, see **Styling** (5.4.5) and **Shadow** (5.4.6).
