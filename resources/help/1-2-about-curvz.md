# About Curvz

Curvz is a vector editor focused on the icon-design workflow.

It exists because the existing options each compromise on something.
Inkscape is excellent but generalist — its UI is built around the
full breadth of SVG, and the icon-specific bits are a small island
inside a much larger app. Illustrator and Affinity Designer are
proprietary. Boxy SVG is web-based. None of them sit comfortably on
Linux as a first-class native tool.

Curvz takes a narrower aim: a desktop-class vector editor whose
defaults, idioms, and output assume you are drawing icons.

## What Curvz is

- **A vector editor.** Bézier paths, boolean operations, stroke and
  fill, transforms, the usual machinery — all native, all GTK.
- **Icon-aware.** Templates default to icon-friendly sizes (16, 24,
  32, 64, 128, 256). Imports normalise long-axis to 1000 on request.
  Exports write `currentColor`-bearing SVG by default. The Tools and
  Inspector are tuned for the workflow of making the same icon at
  several sizes look right at all of them.
- **Project-shaped.** A Curvz project is a directory holding many
  related icon documents — typically a set you intend to ship as a
  family. The project carries its own colour swatches, named styles,
  and themes; documents share them.
- **Linux-native.** Written in C++17 against GTK4 / gtkmm-4.0, no
  Electron, no web view, no sandboxed runtime. Reads and writes
  plain SVG and a small JSON sidecar — your work is portable out of
  Curvz at any time.

## What Curvz isn't

- **Not a generalist illustration tool.** It will draw arbitrary
  artwork, but the geometry primitives, defaults, and exports are
  weighted toward icons. A poster designer would feel constrained.
- **Not a font editor.** Glyphs that look like font glyphs are
  fine, but Curvz does not generate font files. Pair it with
  FontForge or Glyphs for that.
- **Not a raster editor.** It places and references raster images
  but does not paint pixels. Use GIMP or Krita for the bitmap side.
- **Not a multi-page document tool.** No artboards-on-a-page
  layout, no spreads, no magazine model. One document, one canvas,
  one icon — projects gather many such documents into a family.

## Who it's for

People who design icon families on Linux and want a focused tool
that mirrors the way that work actually goes — many small
documents, a shared colour and style language across them, and SVG
output that respects `currentColor` so the icons reflow in whatever
theme renders them.

## Why GTK

Curvz could have been Qt — the author's own background is Qt going
back to TrollTech. The choice came down to licensing: GTK is LGPL
and lets Curvz ship without restrictions on how it is bundled or
distributed. Qt's commercial / GPL split would have constrained
that. GTK's idioms are different, but its licensing is simpler, and
for a Linux-first vector tool that mattered more.
