# Flatpak packaging

This directory contains the Flatpak manifest for building Curvz as a sandboxed
application bundle.

## Files

- **`io.github.curvz.Curvz.yaml`** — the manifest. Declares the GNOME 47
  runtime, three bundled dependencies (spdlog, Clipper2, cmark), and Curvz
  itself.

## Prerequisites

```
sudo dnf install flatpak flatpak-builder       # Fedora
sudo apt install flatpak flatpak-builder       # Ubuntu / Debian

flatpak remote-add --if-not-exists flathub \
    https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Sdk//47 org.gnome.Platform//47
```

The SDK + runtime install is ~700 MB and one-time.

## Build

From the repo root:

```
flatpak-builder --user --install --force-clean \
    build-flatpak packaging/flatpak/io.github.curvz.Curvz.yaml
```

First build: ~5-10 min (compiles spdlog, Clipper2, cmark from source).
Code-change rebuilds: ~30 sec (deps cached).

## Run

```
flatpak run io.github.curvz.Curvz
```

The Curvz icon also appears in your launcher.

## Uninstall

```
flatpak uninstall io.github.curvz.Curvz
```

This is independent of any `cmake --install` install — the two coexist.

## Produce a shareable bundle

To make a single `.flatpak` file installable on any Linux machine:

```
flatpak-builder --repo=build-repo --force-clean \
    build-flatpak packaging/flatpak/io.github.curvz.Curvz.yaml
flatpak build-bundle build-repo curvz.flatpak io.github.curvz.Curvz
```

Users install with `flatpak install ./curvz.flatpak`.

## Sandbox holes

The manifest's `finish-args` grant Curvz:

- Display (Wayland + X11 fallback) and GPU access
- Read/write the user's home directory
- Print via CUPS
- Read-only access to system icon themes (so the in-app icon scanner can
  enumerate the user's installed themes)

Nothing else. No network access at runtime, no access to other apps' data,
no D-Bus calls outside the standard portal set.

## Per-app data path

Inside the Flatpak sandbox, `Glib::get_user_data_dir()` resolves to:

```
~/.var/app/io.github.curvz.Curvz/data/
```

…rather than `~/.local/share/`. Curvz's logs and per-user config land there
when run as a Flatpak. A `cmake --install`'d Curvz still uses
`~/.local/share/curvz/`. The two installs do not share state.
