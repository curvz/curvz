# View options

The **View** menu controls how the canvas is presented while you
edit. None of these settings change the icon you're producing —
they only change what you see while drawing. Toggle rulers to
free up screen space, switch to outline mode for fast structural
checks, jump between zoom levels with single-key presses.

This section walks each one. The actual handlers are wired
directly in the keyboard controller (see the discipline reminder
in the s130 handoff: `set_accels_for_action()` is cosmetic in
this codebase), so the keys below are the authoritative bindings.

![The View menu with Rulers, Outline Mode, and Zoom items](img/10-1-view-menu.png)

## ❶ Rulers

The **Rulers** toggle ❶ shows or hides the canvas's top and left
rulers. Hidden rulers reclaim about 24 pixels on each axis,
which adds up on a small laptop screen. The ruler corner
(the small square where the two rulers meet) hides with them.

Trigger from **View → Rulers** or with `Ctrl + R`. The setting
is per-window — switching documents within the project keeps the
same ruler visibility.

The rulers themselves and their corner-square interactions are
covered in **Canvas, rulers & corner** (3.6). What's done here is
just turning them on or off.

## ❷ Outline mode

**Outline mode** ❷ replaces every path's filled rendering with a
single-pixel stroke. Fills disappear, stroke widths drop to
hairline, and the canvas turns into a wireframe view of the
underlying geometry.

It's invaluable for:

- **Spotting overlapping paths.** Two filled shapes can hide
  each other; in outline mode you see every path's outline
  including the buried ones.
- **Checking node placement.** Without fill obscuring the path,
  you can verify a path's anchors are where you expected.
- **Working at heavy zoom levels** where rendering filled
  geometry slows to a crawl. Outline mode is far cheaper to
  draw.

Trigger from **View → Outline Mode** or with `Ctrl + E`. The
status bar shows `Outline` while the mode is active and
`Preview` when normal. Outline mode is per-document — each
document remembers its own setting.

> **Note:** Curvz refuses to switch into outline mode at extreme
> zoom levels (the drop-shadow buffer sizes that happen behind
> the scenes can crash at very high magnifications). If the
> toggle doesn't fire, zoom out and try again.

## ❸ Zoom

The **Zoom** submenu ❸ holds five preset levels plus
zoom-to-selection and zoom-to-fit. They share a common reference
point — Curvz treats "fit to window" as 1× and the rest as
multiples of that, rather than absolute scale percentages. This
matters because a "100%" reading depends on document size; with
the fit-relative scheme, 1× always shows you the whole artboard
with margin, regardless of how big the artboard is.

The presets:

- **Zoom In** / **Zoom Out** — step up or down by one level.
- **Zoom to 100%** — 1× of fit zoom (the artboard fits the
  viewport with comfortable margin). The menu calls this 100%
  for muscle-memory reasons; the actual ratio is whatever fits
  the artboard.
- **Zoom to 200%** — 2× of fit zoom. One step in from 100%.
- **Zoom to Selection** — fit the current selection's bounding
  box to the viewport.
- **Fit to Window** — same as 100% (the menu provides both
  spellings; the keyboard binds 0 here for muscle memory with
  other tools).

The zoom shortcuts work as both *plain digit* and *Ctrl-digit* —
pick whichever fits your hand. `1` and `Ctrl+1` do the same
thing.

## ❹ Snapping (toolbar, not menu)

Snap toggle isn't in the View menu but it's a view-related
control. The toolbar's snap switch and the inspector's Snap
section (5.3.6) both bind to the same setting. Press `Q` to
toggle from anywhere — same key documented in the Snap leaf.

## What's *not* a view option

A few things that look like view options conceptually but live
elsewhere:

- **Theme** (Dark / Light app chrome) — this is in the
  inspector's **Motif** section (5.2.1), saved per project.
- **Artboard and workspace colours** — also in Motif. They
  affect how the editor looks but aren't transient like outline
  mode.
- **Display unit** (px / in / mm / pt) — this is in the
  inspector's **Canvas** section (5.3.2) per document.

The View menu is for transient editing-time toggles. Things
that should travel with the project go in the inspector instead.

## Where to next

- For the rulers themselves and the corner-square interactions,
  see **Canvas, rulers & corner** (3.6).
- For per-document units (the cousin of zoom — both control
  what numbers you see while editing), see **Units** (10.2).
- For app theme (Dark / Light), artboard colour, and workspace
  colour, see **Motif** (5.2.1).
- For the master keyboard cheatsheet covering every shortcut in
  Curvz, see **Keyboard shortcuts** (11.2).

### Keys

These bindings act on the canvas with the canvas focused.

- `Ctrl + R` — toggle rulers.
- `Ctrl + E` — toggle outline mode.
- `+` or `=` — zoom in one step.
- `-` — zoom out one step.
- `0` or `Ctrl + 0` — zoom to fit (artboard fills viewport).
- `1` or `Ctrl + 1` — zoom to 100% (1× of fit).
- `2` or `Ctrl + 2` — zoom to 200% (2× of fit).
- `3` or `Ctrl + 3` — zoom to selection.
- `Ctrl + Shift + 0` — zoom to fit *all objects*, including
  anything parked outside the artboard.
- `Q` — toggle snap on/off.
- `F1` — open this manual. `Alt + ?` does the same on
  keyboards where F1 is awkward (some Apple Silicon laptops
  default the function row to system controls).
- `?` (no modifier) — open the **Shortcuts dialog**. This is
  the in-app cheatsheet, separate from the manual chapter
  **Keyboard shortcuts** (11.2).
