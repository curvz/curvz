# resources/

Runtime-bundled assets compiled into the Curvz binary by `glib-compile-resources`.

The single source of truth for what gets bundled is **`resources.xml`**. Anything
not listed there is ignored by the build — files in this tree that don't appear
in the manifest are dead weight and should be either added or removed.

## Layout

| Subdirectory | Contents                                                             |
|--------------|----------------------------------------------------------------------|
| `icons/`     | UI iconography. Symbolic icons for the GTK icon theme, plus the few `-dark.svg` / `-light.svg` pairs still loaded directly via `CurvzIcons::make_picture`. |
| `branding/`  | Curvz logos (light/dark). Used in the headerbar and About dialog.    |
| `data/`      | Non-icon runtime data. Currently `seeds.json` (starter-template list).|
| `help/`      | In-app manual markdown sources. Read by `HelpWindow`.                 |
| `palettes/`  | Bundled `.gpl` colour palettes. Listed in the Swatches panel dropdown.|

## Two flavours of icon, two lookup mechanisms

This catches everyone once. Icons are loaded by one of two paths:

**Path A — direct gresource lookup.** Code calls
`Gtk::Picture::set_resource("/com/curvz/app/icons/<name>")`. Works for any path
inside the bundle. Currently used for the layer-row eye icon (via
`CurvzIcons::make_picture`) and the headerbar logo. Files: `icons/eye-*.svg`,
`branding/curvz-logo-*.svg`.

**Path B — GTK icon theme by name.** Code calls
`set_icon_name("curvz-select-symbolic")`. GTK searches its registered icon paths,
which are *rigid* about the freedesktop hicolor layout
(`<search-path>/hicolor/scalable/apps/<name>.svg`). To make this work, on first
launch `Application::on_activate` extracts each symbolic from the bundle and
writes it to `~/.local/share/icons/hicolor/scalable/apps/`, then registers that
directory as a search path. Files: `icons/curvz-*-symbolic.svg`.

The two-block structure of `resources.xml` (one prefix `/com/curvz/app/icons`,
one prefix `/com/curvz/app/icons/scalable/apps`) reflects these two access
patterns. Don't rename the prefixes without updating `Application.cpp` —
`on_activate` looks up icons under the second prefix by hardcoded path.

## Adding new content

1. Drop the file in the appropriate subdirectory.
2. Add a `<file>` line to `resources.xml` under the matching `<gresource>` block.
3. Rebuild — `CMakeLists.txt` GLOBs the subdirs and re-runs `glib-compile-resources`.

For a new symbolic icon, add it to *both* gresource blocks: once under the
`/com/curvz/app/icons` prefix (so it's reachable via direct lookup if needed)
and once under `/com/curvz/app/icons/scalable/apps` with an `alias=` attribute
(so the on-first-run extractor can find it by hicolor-shaped path). Then add
the icon name to the `symbolic_icons[]` list in `Application::on_activate`.
