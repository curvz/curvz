# currentColor & symbolic icons

Curvz is built to produce **symbolic icons** — small, monochrome
SVGs whose colour is decided not by the file but by the
application that displays them. Open a Curvz icon in a GTK or
Qt application set to a dark theme, the icon paints light. Open
the same file in a light theme, the icon paints dark. The icon
data never changes; the runtime decides.

This is the freedesktop "symbolic icon" convention, and it works
through one SVG keyword: `currentColor`. This section is the
conceptual leaf — what `currentColor` is, why Curvz uses it as
the default, and how to keep your icons compatible with the
themes that consume them.

![The same icon rendered against a light and a dark surface](img/9-2-currentcolor-comparison.png)

## ❶ What `currentColor` actually is

Inside an SVG file, a fill or stroke can be:

- A **literal colour** — `#FF8000`, `rgb(255,0,0)`, `red`. The
  paint is fixed; the file determines what shows up.
- The keyword **`currentColor`** — a deferral. The paint
  inherits from the SVG element's `color` property, which is
  inherited in turn from the rendering context. In a CSS world
  that's the surrounding text colour; in a desktop-environment
  world it's the icon-theme colour the system has chosen.
- **`none`** — paint nothing. Used for fills you don't want
  (a stroke-only path) or strokes you don't want.

Most everyday SVGs use literal colours, because they're meant to
look the same everywhere. Symbolic icons are different: their
job is to *blend in*. A literal black icon on a black panel is
useless. `currentColor` solves the problem at the file format
level — the icon defers, the platform decides.

## ❷ Why Curvz defaults to `currentColor`

Open Curvz, draw a path with the Pen tool, and its fill is
`currentColor` by default. So is its stroke. The default is
not a colour; it's the deferral.

This matters for two reasons:

- **You don't have to remember to change the fill before
  exporting.** A path you draw with default settings will be a
  valid symbolic icon. If you replaced the default with, say,
  black, then every icon you forgot to fix would render
  invisibly on a black panel.
- **The Curvz canvas can render `currentColor`-aware icons
  correctly while you edit.** Curvz substitutes a theme-aware
  contrasting colour at draw time so your work-in-progress is
  visible against your chosen canvas backdrop (the **Theme
  disclosure**, 5.3.8). The exported file still says
  `currentColor`; only the on-screen render resolves to a
  literal value.

You can override the default per object. Pick a literal colour
in the inspector's **Styling** section (5.4.5), or bind to a
swatch — and the export will use that. But for symbolic icon
work, leaving everything at `currentColor` is normally the right
choice.

## ❸ How Curvz exports symbolic icons

When you export a Curvz project to a freedesktop icon theme
(see **Import & export**, 2.3), every icon name ending in
`-symbolic.svg` follows the convention:

- Path fills set to `currentColor` are emitted as
  `fill="currentColor"`.
- Path fills set to a literal colour are emitted as that hex
  colour.
- Strokes follow the same rule.
- The file is otherwise plain SVG with no Curvz-specific
  attributes (the `data-curvz-*` round-trip annotations are
  stripped on export).

The result is a clean SVG you can drop into a GTK4 application's
icon set, install into a system theme, or hand to any other
software that follows the freedesktop spec. The paint is
deferred where you want it deferred; consumers handle the rest.

## ❹ Bitmap snapshots

Symbolic icon themes also include PNG renders at standard sizes
— `16x16/`, `24x24/`, `32x32/`, `48x48/`, `64x64/`, `128x128/`,
`256x256/`. Curvz renders these at export time. Because PNG
can't defer paint the way SVG can, the bitmap renders use a
neutral colour (typically a mid-grey) that reads acceptably
against both light and dark surfaces.

PNGs in a symbolic theme are fallbacks for environments that
don't pick up the SVG. On modern desktops the SVG is what
actually renders; the bitmaps are insurance.

## ❺ Mixed icons

Some icons aren't fully symbolic. A status indicator might want
a fixed accent colour (a red dot for an error), or a logo might
have a brand colour the consumer shouldn't override. Mix and
match per path:

- Paths whose colour should follow the theme: leave at
  `currentColor`.
- Paths whose colour is fixed: pick a literal colour or bind to
  a swatch.

The export emits each path's actual setting. A multi-path icon
where the body is `currentColor` and the dot is `#E04C4C` will
render with the system colour for the body and the literal red
for the dot in any consumer.

## ❻ Working in Outline mode

Outline mode (`E` to toggle, see **View options**, 10.1) ignores
fill entirely — every path renders as a thin stroke regardless of
its actual paint. This means a path with `currentColor` fill and
a path with a literal red fill look identical in Outline mode.
The setting is still there; it's just not what's being drawn at
that moment.

If you want to see which paths use `currentColor` versus literal
colour without exporting, toggle back to Preview mode and rely on
Curvz's contrast substitution — `currentColor` paths render in
the substituted theme colour; literal-colour paths render in
their literal colour.

## ❼ Pitfalls

A few things that catch people:

- **Paste-from-elsewhere often comes in as literal colour.** If
  you paste an icon from a stock SVG library or another vector
  editor, its paths probably have literal colours. Open the
  inspector's Styling section (5.4.5), set fill to
  `currentColor`, and the icon becomes symbolic-ready.
- **Some pasted SVGs use CSS classes for colour.** Curvz
  imports them as literal colours. The conversion to
  `currentColor` is the same: pick the path, change fill.
- **`#000000` is not the same as `currentColor`.** A black icon
  exported with literal `#000000` fill will be invisible on a
  black panel in the consuming app.
- **Stroke and fill are independent.** An icon with a
  `currentColor` fill and a hard-coded stroke colour will have
  its stroke not match the fill across themes. If you want both
  to follow the theme, set both to `currentColor`.

## Where to next

- For per-icon paint editing, see **Styling** (5.4.5).
- For exporting a Curvz project as a freedesktop icon theme,
  see **Import & export** (2.3).
- For seeing the icon against a different surface tint while
  you draw, see the **Theme disclosure** (5.3.8) — the
  artboard, workspace, and creation colours are configurable
  per document.
- For a colour library shared across the project's icons, see
  **Swatches** (6.4).
