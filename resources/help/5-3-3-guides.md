# Guides

The **Guides** section governs the document's set of layout
guides — the thin coloured lines you can drag from the rulers to
mark off positions, angles, and alignment baselines. Like grid
and margins, guides are document-scoped, never export with the
SVG, and exist only to help you place artwork precisely.

This section does two things at once: it edits the
**document-wide guide colour**, and — when one or more guides
are selected — it edits **the guides themselves**.

> **Section location:** Guides moved from the Document group to
> the **Object** group in s179. Guides are objects on the canvas
> with a selection model, not a doc-level surface setting, so
> they sit alongside Selection/Blend/Node now. The section
> remains numbered 5.3.3 here for stable bookmarking even
> though its inspector home is in the Object tier.

![The Guides section with one guide selected, showing X/Y/A spinners](img/5-3-3-guides-section.png)

## Section header swatch

Unique among inspector sections, the **Guides** header carries a
small colour swatch beside its title. Click the swatch in the
header to open the colour picker and change the document's guide
colour. The change is live; every guide in the document repaints
immediately.

The colour is a single document-wide value — Curvz does not
support per-guide colours. If you want some guides to read
differently, the convention is to lock or hide the ones you are
not using rather than try to recolour them.

The theme bundle (5.3.8) round-trips the guide colour as part of
its saved palette, so applying a theme reapplies the guide
colour you saved with it.

## ❶ Color row

Inside the section disclosure the same swatch is repeated as a
**Color** row. The two are equivalent; the in-header position is
a glanceable preview, the in-disclosure row is for users who
expect a colour control to live in the section body.

## ❷ Guide selection content

Most of the section's body changes based on what is selected in
the document's guide layer.

### Nothing selected

When no guide is selected, the Color row is followed by a dim
**No guide selected** placeholder. To get content here, click a
guide on the canvas (or in the **Guides** layer in the Layers
panel).

### One guide selected

A single-guide selection shows the full per-guide editor. The
top row reports the guide's **type** and offers two actions:

- **H, V, A** — a single-letter type label. **H** for
  horizontal guides, **V** for vertical, **A** for angled
  (anything else).
- **🔓 / 🔒 lock toggle** — locks just this guide. A locked
  guide cannot be dragged, edited, or deleted via this section.
- **× delete** — removes this guide from the document.

Both action buttons are disabled when the **Guides layer
itself** is locked (locked from the Layers panel). The
guide-level lock is independent of the layer-level lock.

Below the action row are three numeric fields:

- **X:** the guide's anchor point's X coordinate.
- **Y:** the guide's anchor point's Y coordinate.
- **A:** the guide's angle in degrees, measured from the X
  axis. Horizontal guides read 0; vertical read 90.

For horizontal guides only Y is meaningful; for vertical only X
is. Curvz still shows all three for consistency.

All three accept typed values and arrow-key nudges, and respect
the document's display unit. Edits commit on Enter or focus
loss. They are disabled when the guide or its layer is locked.

### Multiple guides selected

When more than one guide is selected the section collapses to a
single row showing the count (`3 guides`) and a **×** delete
button that removes the lot. There are no shared-property fields
— per-guide X/Y/A only makes sense for one guide at a time.

## ❸ From 2 points…

Below the per-guide content is a **From 2 points…** button.
Click it to start a two-click capture: snap the cursor to two
nodes on existing artwork, and Curvz constructs the guide that
passes through both. A small review dialog confirms the result.

Use this when the guide you want is implied by the artwork —
the line through two corners of a shape, the perpendicular to a
stroke at a known node, and so on. It is faster than typing
coordinates by hand.

> **Note:** in the review dialog, press **P** to flip the
> result to the **perpendicular** of the line through the two
> points. Both the original and the perpendicular guide pass
> through the **midpoint** of the two clicks. Useful when the
> two anchors define an *edge* and you want a guide running
> across it — for example, a vertical guide through the centre
> of a shape's top edge.

## Adding guides

You **cannot add a guide from this section**. Guides are
created by dragging from the rulers along the top and left edges
of the canvas, or by the **From 2 points** flow above. Earlier
versions of Curvz had a typed-position add row here; it
duplicated the ruler-drag flow and was removed because users
found it confusing.

If the rulers are hidden (View → Rulers, or **Ctrl+R**), bring
them back to drag a new guide. The ruler corner square has its
own popover for setting the ruler origin if you need that
first — see **Canvas, rulers & corner** (3.6).

## Where to next

Guides are usually paired with **snap** so the cursor catches on
them. Snap is no longer an inspector section — it's a toolbar
popover; right-click the Snap button (Q) for its options.

The other doc-level layout overlays — **Grid** (5.3.4) and
**Margins** (5.3.5) — sit inside the **Theme** disclosure
(5.3.8). Each has its own geometry and its own colour, and all
three (Guides, Grid, Margins) travel together in the theme
bundle.
