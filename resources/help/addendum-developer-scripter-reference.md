# ![Scripter icon](img/icons/face-monkey-symbolic.svg) Scripter reference

This is the **reference** half of the Scripter docs. The
**overview** (previous page) covers what the Scripter is and how
to use it; this section catalogues every Scriptable, verb, and
property that scripts can address.

Reference is broken into sub-pages that follow the same surface
nomenclature as the rest of the manual — there's a page per area
of Curvz that has script-addressable nouns, named the way that
area is named in chapters 3 through 6.

## Sub-pages

- **Language reference** — the grammar, literals, comments,
  subtitles (`#[sub]`), the `result` slot, named variables via
  `set`, the `list` / `do` / `sleep` / `subscribe` / `quit`
  control words, and the trace-output format. Read this first.
- **Singletons reference** — the four standalone Scriptables:
  `proj` (project lifecycle), `export` (headless export
  surface), `app` (process identity), `inspector` (Inspector
  section orchestration + path-safety diagnostics).
- **Header & menus reference** — the `win` action wrapper that
  exposes menu items as verbs, document tab strip, status bar
  readouts.
- **Toolbar reference** — every tool button on the toolbar, plus
  the right-click toolbar popovers (shape defaults, density,
  measure, palette pickers).
- **Inspector reference** — every section of the Inspector,
  matching the §5.x structure of the manual: Selection, Text,
  Blend, Node, Styling, Shadow, Canvas, Guides, Grid, Margins,
  Snap, Measure, Theme, Metadata, Object, Warp.
- **Content reference** — the collection-router Scriptables —
  `layers`, `swatches`, `styles`, `themes`, `palettes`,
  `guides` — and their associated panels (`pnl_styles`,
  `pnl_themes`, ...).
- **Canvas & objects reference** — the `objects` collection for
  the document's on-canvas content, plus the canvas widget's
  own properties.
- **Dialogs & popovers reference** — script-addressable widgets
  in modal dialogs (Blend, Gradient, Style editor, Theme editor,
  Export, Print, Step and Repeat, New document, Save As, …) and
  the toolbar popovers.

## How each page is laid out

Every reference sub-page follows the same shape:

1. **What it addresses** — one paragraph on what the Scriptable
   covers and what part of Curvz it corresponds to.
2. **Addressing** — the registry name (and long-name where
   relevant), plus the per-instance pattern for collection
   Scriptables (`<collection>.<iid>`).
3. **Verbs** — flat list. One bullet per verb: signature,
   one-line gloss, return value (or `Null` for side-effect
   verbs).
4. **Properties** — flat list. One bullet per property: name,
   return type, one-line gloss.
5. **RunContext mask** — which callers may invoke each verb.
   Default `all_three` (TestRunner, Macro, Scripter) is the
   common case; explicit narrower masks called out per-verb
   where they apply.
6. **Examples** — two or three short scripts that exercise the
   surface end-to-end, with the expected trace.

## Conventions across the reference

A few notational rules used throughout:

- **`<arg>`** — required argument.
- **`[<arg>]`** — optional argument.
- **`<verb-name>`** in verb signatures — the name as typed.
- **Return types** — `Bool`, `Int`, `Double`, `String`, or
  `Null` for verbs with no readable return. Lists return as
  newline-separated strings.
- **iid arguments** are written as `<iid>` regardless of which
  collection they belong to — the format is the same (a UUID
  string) for every model object.

Properties that change state (read-after-write) are not flagged
specially — anything you `get` you can also see in the GUI right
after the verb that changed it completes.

If a verb is **not yet wired** but is listed in the source
(future work), the page calls that out inline: *(planned, not
shipped)*.
