# Measure

The **Measure** section configures how the **Ruler tool** (M)
behaves when you measure distances on the canvas. It is small —
two checkboxes — but the behaviour they govern is worth
understanding because the right combination depends on how you
use the tool.

The Ruler tool itself is in the toolbox; this section is just its
preferences. See **Ruler** (4.6.3) for how to take measurements.

![The Measure section showing two checkboxes](img/5-3-7-measure-section.png)

## What "saving" a measurement means

When you complete a measurement with the Ruler tool, Curvz can
do one of two things with the result:

- **Keep it as a transient overlay** — the A/B endpoints and the
  distance label sit on the canvas as a non-printing visual
  until you dismiss them. The measurement is not part of the
  document; closing and reopening loses it.
- **Save it to the Measurements layer** — the measurement is
  promoted to a real document object on a dedicated `Measurements`
  layer. It persists in the SVG (in Curvz's private metadata
  block — never in the exported icon), survives reload, and you
  can have many of them at once.

The first checkbox in this section picks between those two modes.

## ❶ Save measurements

When **on**, every completed measurement is automatically
appended to the Measurements layer. The layer is created on
first save if it does not exist; the measurement persists with
the document.

When **off**, measurements are transient — useful when you are
just sanity-checking distances and do not want to clutter the
document.

This is a workflow preference, not a per-measurement choice. If
you have it off and want to keep one specific measurement,
remember to toggle it on before completing that measurement
(or use the Layers panel to inspect the document's existing
saved measurements after the fact).

## ❷ Delete on copy

The second checkbox is conditional on the first: it only matters
when **Save measurements** is **off**. With save off, every
measurement is transient — and Curvz needs to know what to do
when you copy that transient measurement's distance label.

When **Delete on copy** is **on**: copying the live label (with
**Ctrl+C** or the on-canvas copy gesture) dismisses the
measurement from the canvas. The use case here is "measure,
copy the number into a spinner somewhere, move on" — you do not
want stale measurement overlays piling up.

When **Delete on copy** is **off**: copying the label leaves the
measurement on the canvas. You can keep working with the
overlay visible after copying.

When **Save measurements** is **on**, this checkbox is
**disabled and dimmed** — saved measurements are persistent
document objects, and Curvz does not auto-delete document
objects on a copy operation. Toggle Save off to make Delete-on-
copy meaningful again.

## Picking the right combination

There are three workflows that map onto the four possible
combinations:

- **Save off, Delete-on-copy on** — fast scratch use. Measure,
  read or copy, and the overlay clears itself. Best for casual
  sanity-checking.
- **Save off, Delete-on-copy off** — multiple transient
  measurements. Take several, compare them visually, dismiss
  manually when done. Good for one-shot layout decisions.
- **Save on** (Delete-on-copy doesn't matter) — measurements are
  document content. Best for documentation and review work
  where you want the measurements to survive the session.

The defaults — **Save off, Delete-on-copy off** — match casual
use without surprises.

## Where to next

The Measure section preferences feed the **Ruler tool** (4.6.3).
The Ruler is one of the **Utility tools** in the toolbox; the
others are **Eyedropper** (4.6.1) and **Zoom** (4.6.2).

The **Measurements layer** itself is just an ordinary document
layer once it exists. Use the **Layers** panel (6.2) to lock,
hide, reorder, or delete it like any other.
