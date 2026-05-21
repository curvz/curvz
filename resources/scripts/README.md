# Bundled scripts

Scripts shipped with the Curvz install. Lands at
`/usr/share/curvz/scripts/` (or `/app/share/curvz/scripts/` inside
Flatpak) via the `install()` rule in the root CMakeLists.txt.

The Scripter window surfaces these under a **System** section in its
sidebar (read-only). To edit or modify, use **Save As…** to branch a
copy into the user-scripts directory (`~/.config/curvz/scripts/` by
default, overridable in Preferences ▸ Paths).

## Layout

- `regression/` — cross-cut regression scripts that exercise core
  functionality (project lifecycle, drawing, export, undo/redo, etc).
  Each script is self-contained, reads top-to-bottom, and ends with an
  explicit pass/fail emit. Populated in s267 m3.

Subfolders here become Expander groups in the Scripter sidebar; loose
`*.curvzs` files at this root would land under "All Scripts". Only
`*.curvzs` files are installed — this README and any other supporting
files are filtered out by the `FILES_MATCHING PATTERN "*.curvzs"`
rule in CMakeLists.txt.
