# ![Warp icon](img/icons/curvz-warp-symbolic.svg) Warp

A **Warp** wraps an object in an editable **envelope** — a top
and bottom curve — and deforms the object's geometry to follow
that envelope. The envelope is live: drag its anchor points and
the wrapped object reshapes in real time. The result is a
container node whose visible appearance is the warped version of
its source, while the source itself stays intact inside the
container ready to be edited or recovered.

Warp lives in the **Transforms** section of the toolbar alongside
Align, Blend, Bool, Corner, and Step and Repeat — a block of
selection-modifying operations grouped together because they all
act on the current selection rather than on the canvas at large.

Reach Warp three ways:

- **Toolbar** — the Warp button in the Transforms section makes
  a Warp from the current selection using the current defaults.
  The button is disabled unless the gate below is satisfied.
- **Path → Warp** menu (or `Ctrl + Shift + Y`) — same action.
- **Right-click → Effects → Make Warp** on a warpable canvas
  object — same action.

![A path wrapped in a Warp envelope with anchor points on the top and bottom curves](img/7-6-warp-envelope.png)

## What you can warp

To make a Warp you need exactly **one object selected**, and it
has to be one of these three types:

- **Path** — a plain Bézier path.
- **Compound** — a compound path (multi-contour, with holes).
- **Group** — a group of objects, including nested groups.

Other types (text, images, reference points, clip groups, existing
Blends or Warps) are not warpable. If the selection isn't valid the
toolbar button is greyed out and the menu items are disabled.

Text needs to be converted with **Path → Convert Text to Path**
first; the resulting Path or Compound is then warpable like any
other.

## The envelope

Every Warp has an **envelope** made of two curves: a **top curve**
and a **bottom curve**, each with **2 to 4 anchor points**. The
envelope describes a rectangle-like region that maps onto the
source object's bounding box. As you reshape the envelope, every
point on the wrapped object moves to follow.

![The same path with three different envelope shapes — flat, arched, and waved](img/7-6-warp-presets-comparison.png)

You shape the envelope two ways: pick a **preset** (a named
envelope shape — Flat, Arc Up, Bulge, Wave, and so on) or drag
the **anchor points** directly on the canvas.

### Dragging envelope anchors

When a Warp is selected, its envelope anchors are visible on the
canvas as draggable handles along the top and bottom edges of the
source's bounding box. Click and drag any anchor to reshape that
side of the envelope; the wrapped object reshapes live. Each drag
is one undo step.

Multi-select multiple anchors and drag them together to translate
the whole side at once.

### Presets

The **Preset** dropdown in the Object → Warp inspector section
offers eight named envelope shapes:

- **Flat** — both curves straight, parallel. The identity
  envelope; useful as a starting point for hand-tuning.
- **Arc Up** — top curve arches outward (away from the object
  body); bottom curve stays straight. Letterforms following a
  hill.
- **Arc Down** — top curve straight; bottom curve arches outward.
- **Bulge** — top arches up and bottom arches down. Pillow / barrel.
- **Squeeze** — top arches inward (toward the object body); bottom
  also arches inward. Pinched waist.
- **Perspective Near** — top edge narrower than bottom edge.
  Reads as a billboard tilted away from the viewer at the top.
- **Perspective Far** — bottom edge narrower than top edge.
  The opposite tilt.
- **Wave** — both curves sinusoidal.

Five presets (Arc Up, Arc Down, Bulge, Squeeze, Wave) need
**at least 3 anchors per side** to actually bend — with only
2 anchors there's no interior point to displace, so the
envelope generates as flat. Curvz auto-bumps the anchor counts
to 3 when you pick one of these at counts of 2; Flat and the
two Perspective presets work fine at 2.

Picking a preset **stamps** the envelope with the preset's shape,
overwriting any hand-dragged anchor positions. Once you've dragged
anchors, the dropdown shows **(Custom)** on the next inspector
refresh — that's a "leave the envelope alone" marker, not a
selectable shape. Pick it deliberately and nothing happens; pick
any other preset and the envelope re-stamps.

## ❶ Top and bottom anchor counts

The **Top anchors** and **Bottom anchors** spinners ❶ control
how many anchor points each side of the envelope has, from **2
to 4** independently per side. More anchors give finer control
at the cost of more visual clutter on the canvas; 2 is enough
for a flat or single-arc envelope, 3 is needed for waves and
S-curves, 4 for ribbon-like double waves.

Changing the count regenerates the envelope from the current
preset against the source's bounding box — hand-drag changes are
lost. If you've tuned the envelope by hand, set the counts
first and tune second.

The **Arc Up**, **Arc Down**, **Bulge**, **Squeeze**, and **Wave**
presets need at least 3 anchors per side to actually bend; if
either count is set to 2 when one of these is picked, Curvz
auto-bumps that side to 3 so the envelope has somewhere to bend.
The same auto-bump applies when Make Warp is invoked with one of
these as the default preset.

## ❷ Quality

The **Quality** slider ❷ controls how densely the warped
geometry is subdivided, from **1 to 16**. Default is 4.

Warp works by tracing the source's geometry through the envelope's
coordinate system; higher Quality samples that trace at more
intermediate points, producing smoother curves at the cost of more
work per redraw. For text-sized warps 4 is plenty; for full-page
banner warps 8 or 12 may be needed before the warped curves stop
looking facetted.

Quality only affects the cached warped geometry, not the source —
turning Quality up does not change what's stored, only how finely
it's rendered.

## Defaults vs instance — the dual-mode inspector

The **Object → Warp** inspector section is always visible. Its
binding depends on what's selected:

- **A Warp is selected** — the section edits that instance.
  Spinner and preset changes write to the Warp's own envelope.
  The **Release** and **Flatten** buttons appear at the bottom.
- **Nothing or a non-Warp is selected** — the section edits the
  **application defaults**. The values are what the next new
  Warp will inherit when you press the toolbar button or
  `Ctrl + Shift + Y`. No envelope is generated (there's nothing
  to envelope), and the Release / Flatten buttons are hidden.

This is the "in your face for objects" pattern: configuration is
visible all the time, not buried in a dialog you have to open.
The (Custom) preset entry is suppressed in defaults mode —
defaults can't drift to Custom because there's no instance
envelope to drift away from.

A faster path to the defaults is the **toolbar Warp button
right-click**, which pops a compact defaults editor anchored to
the button. Same four controls, no inspector navigation needed.
Press Esc or click Done to dismiss.

## What gets produced

Hitting **Warp** from the toolbar or menu replaces the source
object in the document with a **Warp container** at the same slot.
The container holds:

- The **source** — your original Path, Compound, or Group,
  unchanged. The source is what you keep editing if you want to
  change what's being warped.
- The **envelope** — top and bottom curves with their anchor
  counts and preset choice.
- A cached **warped geometry** — the source bent through the
  envelope. This is what the canvas paints.

In the Layers panel a Warp appears as a single row. Expanding it
shows the source and the cached geometry as nested rows.

The cached geometry is not directly editable: it's the output of
running the source through the envelope. Edit the **source** to
change what gets warped (open the Warp in the Layers panel, click
the source row, edit normally), or edit the **envelope** to change
how it gets warped (select the Warp, drag anchors or pick a new
preset). Curvz tracks both and rebuilds the cache automatically.

## Release vs Flatten

Two destructive ops convert a Warp back to plain objects:

- **Release Warp** (Path → Release Warp, or right-click →
  Effects → Release Warp) — undoes the warp. The container is
  removed, the source returns to the Warp's slot, and the
  envelope is discarded. The visible geometry on canvas changes:
  what you see is now the unwarped source. Use this when you
  decided you don't want the warp after all.
- **Flatten** (`Ctrl + Alt + F` or Path → Flatten Warp) — bakes
  the warp. The container is removed and replaced by the
  **warped** geometry as a plain Path, Compound, or Group (same
  type as the source). The envelope is discarded but the visible
  result is preserved as ordinary editable geometry. Use this
  when you're done shaping the warp and want to keep the result
  but no longer need the live editing.

Both ops are atomic undo steps. The choice is "keep the original
shape" (Release) versus "keep the warped shape" (Flatten).

## The Edit Warp menu verb

**Path → Edit Warp** (`Ctrl + Alt + Y`) is a navigation aid, not
an editing op. With a Warp selected, the verb makes sure the
inspector's Object → Warp section is current and ready — useful
if the inspector got scrolled or if you arrived at the Warp via
the keyboard. It doesn't open any dialog; the dialog the verb
once opened was retired in s146 when editing moved into the
inspector.

If you press it with a non-Warp selected, nothing happens.

## Undo

Make / Release / Flatten are each one undo step. `Ctrl + Z`
reverts the entire op — for Make, the source returns to the
slot; for Release, the Warp returns intact (envelope and all);
for Flatten, the Warp returns and the baked geometry disappears.

**On-canvas envelope drags** push their own undo step per drag,
so trial-and-error reshaping on the canvas is fully reversible.

**Inspector spinner and preset changes** currently change the
Warp's state live without their own undo entry — they're rolled
into the parent Warp's state and reverted only when the Warp
itself is undone. If you pick a preset and want to revert, the
fastest paths are picking a different preset or dragging the
envelope back; full undo would require undoing the whole Warp
and recreating it.

## Where to next

- The **Blend** verb (7.3) is the conceptual sibling — both Blend
  and Warp wrap objects in a container with live recomputation.
  Blend interpolates *between* two objects; Warp deforms *one*
  object through an envelope.
- The **Compound & derived** topic (8.3) covers all of Curvz's
  derived-container types — Blend, Warp, Clip group, Compound
  path — and how they relate to their sources in the Layers
  panel.
- **Convert Text to Path** (8.3) is the path you take when you
  want to warp text — convert it first, warp the path second.

### Keys

These hotkeys all require canvas focus, and each has its own
selection gate:

- `Ctrl + Shift + Y` — Make Warp. Gated on a size-1 selection
  of a Path, Compound, or Group.
- `Ctrl + Alt + Y` — Edit Warp (refresh inspector focus). Gated
  on the selection being a Warp.
- `Ctrl + Alt + F` — Flatten Warp. Gated on the selection being
  a Warp.

Release Warp is menu / right-click only — by design, since the
verb is destructive of the envelope and a four-finger hotkey
combo is too easy to mis-fire.
