# Snap

The **Snap** section configures which kinds of layout aids the
cursor catches on while you draw or move objects. Snap is
document-scoped — every snap toggle is saved with the document
and synchronises with the project, so a freshly-opened tab keeps
your last preferences.

There is also a **master snap toggle** that lives outside this
section: **Q** on the keyboard, or the snap toggle in the
toolbar. The toggles in this section only matter when the master
is on.

![The Snap section showing six per-kind checkboxes](img/5-3-6-snap-section.png)

## ❶ Six per-kind toggles

Snap is broken into six **kinds**, each with its own checkbox.
Curvz takes the union — turning on more kinds gives the cursor
more places to catch:

- **Snap to guides** — catches on every guide in the document's
  guide layer. The most common toggle to leave on; guides are
  the explicit "I want this aligned" markers.
- **Snap to grid** — catches on the grid lattice from the
  **Grid** section (5.3.4). Useful for icon work on a fixed
  pixel grid.
- **Snap to margins** — catches on the margin frame and any
  column/row gutter lines from the **Margins** section
  (5.3.5).
- **Snap to nodes** — catches on the anchor points of every
  path on the canvas. Useful when you are aligning new artwork
  to existing geometry.
- **Snap to edges** — catches on the bounding-box edges of
  every object. Quicker than node-snap when you just want
  alignment with another object's silhouette.
- **Snap to centers** — catches on the centres of bounding
  boxes. Handy for aligning concentric shapes or placing one
  object's centre on another's edge.

The defaults are **guides + nodes** on, the other four off. That
combination handles most icon work without the cursor feeling
"sticky" everywhere.

## Snap with overlays disabled

The toggles are independent of whether the corresponding
overlay is *visible*. **Snap to grid** with the grid disabled
still catches on a grid that is not drawn — Curvz remembers the
last grid geometry on the document layer even when the layer is
hidden. The same applies to margins.

In practice this is rarely useful. If you want grid snap, enable
the grid; if not, leave both off. Visible-but-not-snapping is
the common case (a grid you can *see* without it pulling on the
cursor).

## How the master toggle interacts

The master snap toggle (**Q**, or the toolbar button) gates the
whole system. With master snap **off**, none of the six kinds
have any effect — the cursor moves freely. With master **on**,
the per-kind toggles decide which classes of target catch.

The master is meant for moments when you need to disable snap
quickly — for nudging a single pixel, sketching free-hand, or
working in a region cluttered with snap targets you do not want
right now. Tap **Q**, do the work, tap **Q** again to restore.

## Project-document sync

Snap settings are saved both in the document and at the project
level. Editing them through this section updates both
simultaneously: the toolbar reads the same values, the next
document you open in the project starts with the same settings,
and saving and reloading round-trips cleanly.

If you are switching between documents in the same project and
expecting different snap behaviour for each, that is not how it
works — the project copy is the canonical one, so editing snap
in any document propagates to the others. This is intentional;
snap preferences are about how *you* work, not about a
particular document's content.

## Where to next

The Snap section pairs with three other Document-group
sections: **Guides** (5.3.3), **Grid** (5.3.4), and **Margins**
(5.3.5). Each defines geometry; this section decides which the
cursor honours.

If you find yourself toggling **Q** constantly while working,
check whether the per-kind toggles are over-broad. The right
mix lets you keep master snap on permanently, with **Q** as the
escape hatch rather than the default state.

### Keys

- `Q` — toggle master snap on/off (canvas focus required).

The Q key only fires when the canvas has focus. If you have
typed into a spin button or text entry, click the canvas once
or press **Esc** before pressing Q.
