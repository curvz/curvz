# ![Text on Path tool icon](img/icons/curvz-text-on-path-symbolic.svg) Text on Path

The **Text on Path tool** attaches an existing text run to an
existing path so the type follows the curve. It's a linker — both
the text and the path must already exist before you start. The
tool's job is to connect them, then let you slide the start point
along the path with a draggable handle.

Activate it from the toolbox or with the **U** key (canvas focus
required).

For text-on-path *after* it's linked — offset value, flip side,
detach — the **Text** inspector section (5.4.2) has the numeric
controls.

![A text run flowing along a curve, with the I-beam handle at its start](img/4-5-2-text-attached.png)

## The two-phase flow

The tool walks through up to three phases per session, identified
internally as **phase 0**, **phase 1**, and **phase 2**:

### Phase 0 — pick the text

Click on a text node. Curvz picks it up, marks it the *target*,
and advances to phase 1.

If the clicked text is **already attached** to a path, the tool
skips phase 1 entirely and lands in phase 2 — letting you drag
the start offset directly. Useful for re-tuning where the text
starts.

### Phase 1 — pick the path

With a text node held from phase 0, click on a path. Curvz links
them: the text is moved off its free position and onto the path,
starting at offset zero (the path's first node), running along
the path's natural direction.

If the picked path doesn't have a stable internal id yet, Curvz
gives it one — the link is by id, not by pointer, so it survives
file save/reload.

A short warning surfaces if the text content has more than one
line: text-on-path uses single-line layout only, so only the
first line will render along the curve.

### Phase 2 — drag the start point

A small **I-beam handle** appears on the path at the text's
current start offset. Drag it along the path to slide the text's
starting point. The text re-flows live as you drag, glyphs
following the curve at every position.

Click the path stroke (anywhere along it, not just the handle)
to start the same drag — the path itself is also draggable in
phase 2 for less fiddly control.

A miss in phase 2 (clicking neither the handle nor the path) is
a no-op — the link stays intact, you stay in phase 2.

## Detaching and releasing

Two paths to undo a text-on-path link:

- **Detach** — keeps the text where it currently sits along the
  path, but breaks the link. The text becomes free-floating
  again, with `text_x` and `text_y` set to the visual position
  it had on the path.
- **Release** — same idea, applies to the whole selection (so
  multi-text-on-path workflows can release in batch). The
  shortcut is **Shift + U** when the canvas has focus.

The Detach button lives in the **Text** inspector section
(5.4.2), shown only when the selection is text-on-path. Detach is
the affordance you'll use most often; Shift + U release is for
when you've selected several linked text nodes at once.

## Right-click for the editor

Right-clicking with the Text on Path tool active opens a small
context menu over the hit object. The menu's contents depend on
what was hit (link / detach / cancel are the main verbs).

For most workflows the click flow above is enough; the right-click
is a fallback when you want to reach a verb without dragging.

## What gets stored on the text node

After a successful link, four fields on the text node carry the
text-on-path state:

- **`text_path_id`** — the linked path's stable internal id.
  Survives save/reload.
- **`text_path_offset`** — distance along the path (in document
  units) from the first node to the text's start.
- **`text_path_flip`** — false by default; true mirrors the text
  to the other side of the path. Toggle from the **Below path**
  checkbox in the Text inspector section.
- **`text_x` / `text_y`** — preserved from the pre-link state.
  When you Detach, these are restored as the new free position.

The tool is a thin layer over these fields; everything you do
with U eventually shows up in the inspector as values you can
edit numerically.

## Limits and gotchas

- **Single-line text only.** Multi-line text content (with
  newlines) is not supported on a path; only the first line
  renders. Curvz warns at link time if your text has newlines.
- **The path must be a real path object**, not a group, compound,
  or other container. If you have a complex path that happens to
  be wrapped in a group, ungroup first or pick the inner path
  via the Layers panel.
- **The path's direction** dictates the text's flow direction.
  Reverse the path (Node tool, R) to reverse the text's flow
  along it.
- **Closed paths work.** A closed path treats the offset as a
  position around the loop; advancing past the path's length
  wraps back to the start.

## Where to next

For the inspector controls that fine-tune offset, flip, and
detach, see the **Text** inspector section (5.4.2). For the Text
tool that creates the text in the first place, see **Text**
(4.5.1).

If you've drawn the path with the Pen and want to attach text
without leaving keyboard hands: T to switch to Text tool, click
to drop and type, U to switch to Text on Path, click the text,
click the path. Three tool switches, two clicks per attachment.
