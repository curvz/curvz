# Themes

The **Themes** panel saves the active document's per-document
surface settings — Canvas colours, Margins, Grid, Guides, display
unit, and snap settings — as a named **theme**, and applies that
theme to one or more documents in the project. Use it to keep a
consistent look across an icon family, to flip a set of icons
between editing-time presentations, or to package a configuration
once and reuse it across projects.

Themes are not bindings. Applying a theme writes its values into the
target documents and the link ends there — editing the theme later
does not retroactively repaint anything you previously applied it
to. The model is *apply and forget*.

> **Section location:** Themes sits between **Styles** (6.5) and
> **Documents** (6.6) in the Content pane's stack. It is numbered
> 6.7 in this manual to keep the existing 6.1–6.6 file numbers
> stable.

![The Themes panel with three saved themes and the apply-targets list](img/6-7-themes-panel.png)

## What a theme bundles

A theme captures every doc-level "non-physical" setting in one
record:

- **Display unit** — px / in / mm / pt.
- **Canvas colours** — artboard, workspace, and creation, each
  with separate values per app appearance mode (Dark / Light).
- **Guide colour** — the document-wide guide colour.
- **Grid** — enable state, style (lines / dots), spacing, offset,
  colour.
- **Margins** — enable state, the four insets, column and row
  counts and gaps, colour.
- **Snap settings** — the per-kind toggles from the Snap toolbar
  popover.

The document's *physical* geometry — Canvas width, height, DPI,
aspect ratio, display mode — is **not** part of the theme.
Themes describe presentation, not the page's geometric bones, so
applying a theme never silently resizes the artboard.

Layer structure, paths, and any other artwork content are
likewise outside the theme — themes work on settings, not on the
drawing.

## ❶ Library list

The top of the panel ❶ lists every saved theme as one row. Each
row shows the theme's name, with three action buttons on the right:

- **✎ Rename** — opens a text prompt to rename the theme. Empty
  names are rejected.
- **⎘ Duplicate** — copies the theme under a fresh name (the
  original's name with a numeric suffix appended).
- **✕ Delete** — removes the theme from the project. Undoable.

Click anywhere on a row (outside the action buttons) to **select**
the theme as the source for the next Apply. The selected row is
highlighted, and the Apply button at the bottom of the panel
becomes sensitive once a source is picked.

The library is empty in a fresh project. The current implementation
ships no built-in themes — the two-tier (built-in + user)
architecture is in place but the built-in list is empty in v1, so
every theme you see is one you saved.

## ❷ Library footer — Save and Import/Export

Below the library list are two affordances:

- **[+] Save current as theme** — captures the active document's
  current settings (the seven categories listed above) and saves
  them as a new theme. Curvz prompts for a name; the new theme
  appears in the library list immediately.
- **[⋮] kebab menu** with:
  - **Import…** — load a `.json` theme pack from disk. Names
    that clash with existing themes get a numeric disambiguator.
  - **Export user themes…** — write every user-tier theme out as
    one `.json` pack for sharing or backup. Defaults are saved
    under `~/.config/curvz/themes/`.

Library mutations (rename, duplicate, delete, save-new, import)
push commands onto the project's history — Ctrl+Z reverts them.
Import is one composite undo step regardless of how many themes
the pack contained.

## ❸ Apply targets

Below the library is the **Apply to** list ❸ — one checkbox per
document in the project. Tick the documents you want to apply the
selected theme to; untick to skip. New documents added to the
project later default to ticked.

The Apply button at the bottom of the panel becomes sensitive
when both a source theme is selected and at least one target is
ticked.

## ❹ Apply

Click **Apply** ❹ to run the apply across every ticked document.
A confirmation modal appears listing the source theme and the
targeted documents; confirm to run it, cancel to back out.

The hint label above the Apply button reads
**"Applying a theme is not undoable"** — this is the panel's main
safety affordance. Apply atomically writes every field in the
bundle to every target document, and there's no `bound_theme`
field on the document side that would let Ctrl+Z reverse it.

Why apply isn't undoable: it's a one-shot write across an arbitrary
number of documents, and snapshotting every targeted doc's
pre-apply state into a composite undo would be heavy for a flow
that gets used infrequently. The confirmation modal is the safety
net.

If you need a guarded apply, save the active document's current
state as a backup theme first (the [+] button), then apply the new
theme. If you need to revert, apply the backup theme to the same
targets.

## ❺ The relationship to the inspector

The Theme **disclosure** in the inspector (5.3.8) is the live edit
surface for the active document's settings — change the artboard
colour, the grid spacing, the margin insets, and you're editing
the values that *would* be saved if you hit [+] here.

The Themes panel is the *saved* counterpart. Inspector edits affect
the active document only; saving here promotes those values into a
named, reusable record; applying here writes the record back to
documents you pick.

A typical flow:

1. Edit the active document's Theme-disclosure settings until they
   look right.
2. Hit [+] in the Themes panel to save them as a named theme.
3. Tick the targets in **Apply to** and click **Apply** to push
   those settings to siblings.

## Project scope and persistence

The Themes library is project-scoped. Themes are saved in
`project.json` alongside Swatches and Styles, and travel with the
project when you copy it to another machine.

The JSON pack format used by import/export is project-defined (not
GIMP-derived) and matches the shape of Style packs — readable as
plain text, easy to hand-edit if needed.

## Where to next

- The **Theme** disclosure (5.3.8) — the in-inspector edit
  surface for the active document's theme settings.
- **Swatches** (6.4) and **Styles** (6.5) — the other
  project-scoped libraries. Swatches are colours, Styles are
  per-object appearance bundles, Themes are per-document
  surface bundles. Each is named, exportable, and applyable.
- **Themes** (9.1) — the conceptual chapter on themes,
  workflows, and rationale.
