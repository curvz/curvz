cmake_minimum_required(VERSION 3.20)

# ── Policy: CMP0135 — FetchContent extract-timestamp behaviour ────────────────
# CMake 3.24+ prefers extraction-time timestamps over archive timestamps for
# URL-based FetchContent so that dependents rebuild whenever the URL changes.
# Setting NEW silences the per-configure dev warning and gives us the saner
# behaviour. The if(POLICY ...) guard makes this a no-op on older CMakes
# (we still target a 3.20 minimum).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# ── Compilers — set BEFORE project() so CMake's auto-detection picks them up
# on first configure. Setting them after project() triggers a "variables have
# changed" cache invalidation loop on subsequent configures, which manifests
# as CMake endlessly re-running itself.
#
# Default to clang on systems where it's the natural choice (Fedora aarch64
# dev box), but yield to anything the build environment has already set —
# either via CC/CXX env vars or via a prior cache entry. This lets the same
# CMakeLists work natively under clang AND inside the GNOME Flatpak SDK
# (which ships GCC, not clang).
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER   "/usr/bin/clang"   CACHE STRING "C compiler")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER "/usr/bin/clang++" CACHE STRING "C++ compiler")
endif()

project(curvz VERSION 0.9.0 LANGUAGES C CXX)

# ── Application identity ──────────────────────────────────────────────────────
# Single source of truth for the freedesktop application ID. Used at install
# time to name the .desktop file, metainfo, MIME XML, and the hicolor app icon.
# Must match the string passed to Gtk::Application(...) in src/Application.cpp.
set(CURVZ_APP_ID "io.github.curvz.Curvz")

# ── Parallel build — use all available cores by default ───────────────────────
include(ProcessorCount)
ProcessorCount(CPU_COUNT)
if(CPU_COUNT GREATER 1)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${CPU_COUNT} CACHE STRING "Parallel build jobs")
endif()

# ── Build type — default to Debug for development ─────────────────────────────
# Override at configure time with: cmake -B build -DCMAKE_BUILD_TYPE=Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# s219 m1 — the CURVZ_DIAGNOSTIC option is gone. The Scripter window,
# its listener, the model Scriptables (LayersScriptable / GuidesScriptable),
# and the panel-as-Scriptable (StylesPanelScriptable) now compile into
# every build unconditionally. Whether the Scripter actually opens at
# runtime is a user preference (AppPreferences::scripter_enabled), wired
# to a "Developer ▸ Scripting" menu item and an Application ▸ Developer
# inspector switch. Production users who never flip the toggle pay
# nothing beyond the binary size of a few extra TUs.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# s135 m3: silence GCC's -Wpsabi advisory notes. These fire when GCC's
# calling convention for a particular type differs between compiler
# versions (e.g. std::pair<double,double> changed in GCC 10.1 to match
# C++14 even when C++17 is enabled). The note is purely informational
# and aimed at distro maintainers shipping ABI-stable libraries who mix
# objects across compiler versions. Curvz is built and shipped as a
# single binary (native or Flatpak) where every translation unit is
# compiled by the same compiler — there is no ABI boundary where this
# matters. Targeted suppression keeps the rest of the warning set live;
# the next dev shouldn't have to re-derive why this one was silenced.
# Clang doesn't emit -Wpsabi so the flag is GCC-only.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-psabi)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(GTKMM    REQUIRED gtkmm-4.0)
pkg_check_modules(CAIRO    REQUIRED cairo)
pkg_check_modules(CAIROMM  REQUIRED cairomm-1.16)
pkg_check_modules(PANGOMM    REQUIRED pangomm-2.48)
pkg_check_modules(PANGOCAIRO REQUIRED pangocairo)
pkg_check_modules(PANGOFT2   REQUIRED pangoft2)
pkg_check_modules(FREETYPE   REQUIRED freetype2)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(PNG      REQUIRED libpng)
pkg_check_modules(GIO      REQUIRED gio-2.0)

# nlohmann/json — try system package first, fall back to FetchContent
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# spdlog — try system package first, fall back to FetchContent
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
        URL_HASH SHA256=f89ffd9604b98e4b4c78e95f29e92c31cda4bdb94a1fa6e5b22b4bfee12ac47d
    )
    set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

# Clipper2 — polygon clipping library (Angus Johnson, Boost Software License 1.0).
# Used for compound-capable boolean path operations. The library lives in the
# CPP/ subdirectory of its source tree (SOURCE_SUBDIR). Tests / examples /
# utilities are disabled because we only need the core Clipper2 static lib.
# USINGZ is OFF because we don't use Z-callback features; this keeps only one
# variant of the library built.
find_package(Clipper2 QUIET)
if(NOT Clipper2_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Clipper2
        URL https://github.com/AngusJohnson/Clipper2/archive/refs/tags/Clipper2_1.5.4.tar.gz
        URL_HASH SHA256=9d8a35a29d04cd1b7b45f542c0ba48015feece1210036ea9e4efaad3140af4b0
        SOURCE_SUBDIR CPP
    )
    set(CLIPPER2_UTILS    OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_TESTS    OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_USINGZ   "OFF" CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(Clipper2)
endif()

# cmark — CommonMark parser (John MacFarlane, MIT). Used by HelpWindow to
# parse the in-app manual's markdown sources into a Gtk widget tree. Try
# system package first, fall back to FetchContent.
#
# Target naming notes:
#   • Modern cmark (0.31+) uses standard BUILD_SHARED_LIBS; the resulting
#     target is simply `cmark`. Older `CMARK_STATIC`/`CMARK_SHARED` knobs
#     are deprecated and emit dev warnings.
#   • System find_package may export `cmark::cmark` or `cmark::cmark_static`.
# We normalise to CURVZ_CMARK_TARGET so the link line below is stable.
find_package(cmark QUIET)
if(cmark_FOUND)
    if(TARGET cmark::cmark_static)
        set(CURVZ_CMARK_TARGET cmark::cmark_static)
    elseif(TARGET cmark::cmark)
        set(CURVZ_CMARK_TARGET cmark::cmark)
    else()
        message(FATAL_ERROR "cmark found but no usable target")
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(
        cmark
        GIT_REPOSITORY https://github.com/commonmark/cmark.git
        GIT_TAG        0.31.1
        GIT_SHALLOW    TRUE
    )
    set(CMARK_TESTS    OFF CACHE BOOL "" FORCE)
    # Modern cmark uses BUILD_SHARED_LIBS to choose static vs shared. We want
    # static so the manual parser is baked into the binary alongside the
    # bundled help/.md files (no runtime dep, single-binary distribution).
    # Save/restore the global so this override only affects cmark — any
    # FetchContent deps declared after this point keep whatever the project
    # had set before.
    set(_curvz_saved_bsl ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cmark)
    set(BUILD_SHARED_LIBS ${_curvz_saved_bsl} CACHE BOOL "" FORCE)
    unset(_curvz_saved_bsl)
    set(CURVZ_CMARK_TARGET cmark)
endif()

# ── GLib resources ───────────────────────────────────────────────────────────
# Compile resources.xml → src/curvz-resources.c + include/curvz-resources.h
# Output goes into the source tree (src/ include/) which always exist.
# Adding a new SVG: add it to resources/ and resources.xml, then rebuild.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(RESOURCES_XML "${CMAKE_SOURCE_DIR}/resources/resources.xml")
set(RESOURCES_C   "${CMAKE_SOURCE_DIR}/src/curvz-resources.c")
set(RESOURCES_H   "${CMAKE_SOURCE_DIR}/include/curvz-resources.h")

file(GLOB RESOURCE_FILES
    "${CMAKE_SOURCE_DIR}/resources/icons/*.svg"
    "${CMAKE_SOURCE_DIR}/resources/branding/*.svg"
    "${CMAKE_SOURCE_DIR}/resources/data/*.json"
    "${CMAKE_SOURCE_DIR}/resources/data/*.db"
    "${CMAKE_SOURCE_DIR}/resources/help/*.md"
    "${CMAKE_SOURCE_DIR}/resources/help/img/*.png"
    "${CMAKE_SOURCE_DIR}/resources/palettes/*.gpl")

add_custom_command(
    OUTPUT  "${RESOURCES_C}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${RESOURCES_C}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --target "${RESOURCES_C}"
            --generate-source
            resources.xml
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/resources"
    DEPENDS "${RESOURCES_XML}" ${RESOURCE_FILES}
    COMMENT "Compiling GLib resources (source)"
    VERBATIM
)

add_custom_command(
    OUTPUT  "${RESOURCES_H}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${RESOURCES_H}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --target "${RESOURCES_H}"
            --generate-header
            resources.xml
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/resources"
    DEPENDS "${RESOURCES_XML}" ${RESOURCE_FILES}
    COMMENT "Compiling GLib resources (header)"
    VERBATIM
)

# Force both files to be generated before any source is compiled
add_custom_target(curvz_resources_gen DEPENDS "${RESOURCES_C}" "${RESOURCES_H}")

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/Application.cpp
    src/MainWindow.cpp
    # s164 m1 — MainWindow.cpp split (see top of include/MainWindow.hpp for the
    # category vocabulary and per-method index). Originally one ~7000-line TU;
    # split into categorized TUs that all compile into the same Curvz::MainWindow
    # class. MainWindow.cpp itself is now ~375 lines of glue (ctor + cross-cutting
    # orchestrators); the meat lives in the files below.
    # s165 m1 — sub-TU consolidation. The s164 ship had per-domain sub-TUs
    # (MainWindow_zones_headerbar.cpp, _menu.cpp, _layout.cpp, _overlays.cpp;
    # MainWindow_handlers_documents.cpp, _edit.cpp, _library.cpp, _effects.cpp,
    # _guides.cpp, _macros.cpp). Project rule: any MainWindow_<bucket>_<sub>.cpp
    # collapses into MainWindow_<bucket>.cpp. The 10 sub-TUs merged back into
    # MainWindow_zones.cpp and MainWindow_handlers.cpp; sections within each
    # are demarcated by banner comments and named in the per-method category
    # tags' parenthetical (e.g. `// category: handler (documents)`).
    src/MainWindow_zones.cpp
    src/MainWindow_bindings.cpp
    src/MainWindow_handlers.cpp
    src/MainWindow_helpers.cpp
    src/NewDocumentDialog.cpp
    src/OffsetPathDialog.cpp
    src/TranslateDialog.cpp  # s205 m4 — pivot-aware transform hub
    src/GradientDialog.cpp
    src/SaveAsTemplateDialog.cpp
    src/ManageTemplatesDialog.cpp
    src/ShortcutsDialog.cpp
    src/HelpWindow.cpp
    src/StepRepeatPopover.cpp
    src/BlendPopover.cpp
    src/WarpPopover.cpp
    # s146 m3: WarpDialog removed — warp creation seeds from AppPreferences
    # defaults (inspector Application ▸ Warp), editing lives in the
    # always-visible Object ▸ Warp inspector section.
    # s147 m3: ThemesDialog removed — ThemesPanel in Content is the
    # canonical surface (always-visible, kill-the-commit-gate per the
    # discovery-not-commit principle). Scott deletes
    # include/ThemesDialog.hpp and src/ThemesDialog.cpp on his end.
    src/MacroSystem.cpp
    src/MacroManagerWindow.cpp
    src/MacroEditorWindow.cpp
    # s139 m2: app-tier user preferences (boolean cleanup toggle and
    # whatever future app-tier knobs land). Persists at
    # ~/.config/curvz/preferences.json. Sibling to MacroSystem in
    # role: singleton, load on startup, save on every change.
    src/AppPreferences.cpp
    # s144 m3: most-recently-used .curvz project paths. Separate from
    # AppPreferences (different kind of state — high-churn list vs slow
    # settings) and persists at ~/.config/curvz/recent_projects.json.
    # Drives the File ▸ Open Recent submenu.
    src/RecentProjects.cpp
    src/Canvas.cpp
    src/Canvas_input.cpp  # s161 — Canvas split, INPUT TU (event state machines)
    src/Canvas_ops.cpp    # s161 — Canvas split, OPS TU (boolean/blend/warp/etc.)
    src/Canvas_draw.cpp   # s161 — Canvas split, DRAW TU (all draw_*, on_draw)
    src/SelectionContext.cpp  # s158 m1 — capability classifier (Info+Actions)
    src/DocumentGallery.cpp
    src/PropertiesPanel.cpp
    src/CurvzColorPicker.cpp
    src/ColorPickerRecents.cpp
    src/ColorPickerPopover.cpp
    src/PaintEditor.cpp
    src/StyleEditorDialog.cpp
    # s183 m2 — Theme editor dialog. Peer to StyleEditorDialog,
    # opened from the Themes panel's row Edit (✎) button. Self-
    # deletes via signal_close_request → signal_idle.
    src/ThemeEditDialog.cpp
    # s210 m1 — Image-info read-only properties dialog. Lifted out of
    # Canvas.cpp's right-click handler into its own class, mirroring
    # the s200/s201 hide-on-close singleton architecture. Was the last
    # raw new-Gtk::Window self-deleting dialog in Canvas.cpp; the
    # Close button was the last raw Gtk::Button in that file.
    src/ImageInfoDialog.cpp
    # s210 m2 — Rotate-from-point dialog (R-held + right-click flow).
    # Lifted out of Canvas_input.cpp; same hide-on-close singleton
    # pattern as ImageInfoDialog. Apply routes through Canvas::
    # apply_rotate_from_point (extracted public method) via a
    # CommittedFn closure threaded through MainWindow.
    src/RotateFromPointDialog.cpp
    # s203 m1 — View Clipboard mini float. Edit ▸ View Clipboard…
    # opens a small chromeless window showing the current system
    # clipboard contents so the user can select a piece and Ctrl+C
    # it. Motivated by the Measure tool's structured-block copy.
    src/ClipboardViewWindow.cpp
    src/PreviewPanel.cpp
    src/ContextBar.cpp
    src/DocTabBar.cpp
    src/LibraryPanel.cpp
    src/LayersPanel.cpp
    src/SwatchesPanel.cpp
    src/StylesPanel.cpp
    # s147 m3: Themes Content panel — replaces ThemesDialog. Library +
    # apply-to-targets-with-confirmation in the right pane Content group.
    src/ThemesPanel.cpp
    src/TemplateLibrary.cpp
    src/SeedTemplates.cpp
    src/Toolbar.cpp
    src/StatusBar.cpp

    # Rulers
    src/Ruler.cpp

    # Document model
    src/CurvzDocument.cpp
    src/CurvzProject.cpp
    src/GlyphObject.cpp
    src/Layer.cpp

    # Bezier math layer (pure C++, no GTK/Cairo dependency)
    src/math/Vec2.cpp
    src/math/CubicSegment.cpp
    src/math/BezierPath.cpp
    src/math/BooleanOps.cpp
    src/math/BooleanOpsClipper.cpp
    # s139 m1: Clipper2 output reconstruction post-pass. Header published,
    # keeper-set computation implemented. s139 m2: cleanup_loop body
    # implemented; wired into Canvas::boolean_op behind the
    # AppPreferences::boolean_cleanup_enabled toggle (default off).
    # See docs/clipper-reconstruction-integration.md.
    src/math/BooleanOpsRefit.cpp
    src/math/CornerTreatment.cpp
    src/math/HitTest.cpp
    src/math/PathOffset.cpp

    # Color system. See ARCHITECTURE.md "Color System".
    # Phase 1: atoms + interop with existing FillStyle. No renderers migrate
    # yet; old UI continues to use FillStyle throughout.
    #
    # Color.cpp is pure C++ (no GTK/Cairo). FillStyleInterop.cpp pulls
    # SceneNode.hpp (for FillStyle), which transitively pulls glib for the
    # UUID generator — same dependency tier as the math layer.
    # Paint.cpp (Phase 5 M1) holds resolve_paint — the single entry point
    # for turning any Paint variant into a concrete Color. Depends on
    # SwatchLibrary for SwatchRef resolution.
    src/color/Color.cpp
    src/color/Paint.cpp
    src/color/FillStyleInterop.cpp
    src/color/SwatchLibrary.cpp
    # GplFormat (S101 m1) — GIMP palette parser/writer. Pure format seam,
    # no UI, no SwatchLibrary coupling. Imports/exports translate between
    # GplPalette records and Curvz Palette + Swatch types at the m3 layer.
    src/color/GplFormat.cpp
    # PaletteIO (S101 m3) — bridges GplFormat with SwatchLibrary. Reads
    # bundled palettes from gresource (/com/curvz/app/palettes/*.gpl),
    # translates GplPalette ↔ SwatchLibrary's Palette + Swatch records.
    # Only this layer depends on both the format module and the library
    # — UI calls PaletteIO, never GplFormat directly.
    src/color/PaletteIO.cpp
    # ColorRegion (S83 m4h v2) — pure helper, hex/rgb → human-readable
    # region name ("Vivid Red", "Dark Blue", "Brown", "Beige", "Grey").
    # First consumer is the inspector's swatch-binding annotation
    # fallback when swatch.header.name is empty. Future consumers:
    # auto-name-at-birth invariant on SwatchLibrary::add_swatch and
    # StyleLibrary::add_style; default-swatch authoring (M6 polish).
    src/color/ColorRegion.cpp

    # Style system. See ARCHITECTURE.md "Style System".
    # S74 Phase 1 m1: data model + two-list (app + user) registry +
    # mutate-appearance funnel. App styles are a hardcoded 3-stub list
    # at StyleLibrary construction; no JSON, no UI yet, no SceneNode
    # bound_style field yet (m3). The funnel takes bound_style by
    # explicit reference until m3 promotes it to a SceneNode field.
    src/style/StyleLibrary.cpp
    src/style/StyleInterop.cpp
    # S102 m1: import/export bridge for user-tier style packs as JSON.
    # Mirrors color/PaletteIO's role for palette interchange.
    src/style/StyleIO.cpp

    # S103 m1: theme system — named bundles of doc-level "non-physical"
    # settings (units, background, guides, grid, margins, snap). Sibling
    # of the style system; mirrors its two-list architecture. v1 has no
    # built-in app themes, only user themes (thm_<uuid>). Persisted
    # inline in project.json; m2 will add JSON pack import/export.
    src/theme/ThemeLibrary.cpp
    # S103 m2: import/export bridge for user-tier theme packs as JSON.
    # Mirrors style/StyleIO's role for style interchange.
    src/theme/ThemeIO.cpp

    # SVG serialisation
    src/SvgWriter.cpp
    src/SvgParser.cpp
    src/SvgOptimiser.cpp

    # Tools
    src/tools/SelectionTool.cpp
    src/tools/NodeTool.cpp
    src/tools/PenTool.cpp
    src/tools/RectTool.cpp
    src/tools/EllipseTool.cpp
    src/tools/LineTool.cpp
    src/tools/TextTool.cpp
    src/tools/EyedropperTool.cpp

    # Export
    # s179 m1: unified Export dialog (File ▸ Export…). Tabbed parent
    # for Documents + Icon Theme. Replaced the s103-era ExportDialog
    # (icon theme) and S104-era ExportDocsDialog (per-doc) — the
    # migration completed via additive-prove-retire-consolidate.
    src/ExportDialog.cpp
    src/PngExporter.cpp
    src/IcoExporter.cpp
    src/GResourceExporter.cpp
    src/RefptExporter.cpp  # s176 m1: refpt-coordinates sidecar export

    # System icon scanner
    src/SystemIconScanner.cpp

    # Undo/redo
    src/CommandHistory.cpp

    # Logging
    src/CurvzLog.cpp

    # Utilities — small, composable static pump methods in curvz::utils.
    # SVG-id codec lives here (S96); future utilities accumulate alongside.
    src/curvz_utils.cpp

    # s186 m2 / s187 m1 / s219 m1: script-driven substrate. Originally
    # the listener and Scripter window were CURVZ_DIAGNOSTIC-gated; s219
    # m1 collapsed the gate into a user preference, so every scripting
    # TU now compiles unconditionally. Whether the Scripter opens at
    # runtime is decided by AppPreferences::scripter_enabled.
    # See ARC.md "The Script-Driven Curvz programme" for the design.
    src/scripting/ScriptableRegistry.cpp
    src/scripting/WidgetNameResolver.cpp
    src/scripting/ScriptListener.cpp
    src/scripting/ScripterWindow.cpp
    # s244 m1 — path containment utility for sensitive scripted verbs.
    # Free functions in curvz::scripting; init_roots() runs once at
    # Application::on_startup, path_is_safe() is called by every
    # headless-verb singleton (m5b) before accepting a path argument.
    # See CANON.md "Path containment for sensitive verbs."
    src/scripting/path_is_safe.cpp
    # First concrete PanelScriptable subclass — wraps StylesPanel under
    # abbrev "pnl_styles".
    src/scripting/StylesPanelScriptable.cpp
    # Row-bound model Scriptables. Wrap the project's layer / guide /
    # swatch / style / theme / objects collections under abbrevs
    # "layers" / "guides" / "swatches" / "styles" / "themes" /
    # "objects" with transient per-instance proxies (`layer.<iid>`,
    # `guides.<iid>`, `swatches.<id>`, `styles.<id>`, `themes.<id>`,
    # `objects.<iid>`) routed through the Scriptable::can_resolve /
    # proxy_for hooks. The swatches one (s221 m1) is the first
    # library-collection Scriptable, styles (s222 m1) is the second,
    # themes (s223 m1) is the third; the first two wrap SceneNode
    # collections of top-level layer-like slots. The objects one
    # (s230 m1) is the largest, wrapping every "real scene content"
    # SceneNode (Path / Group / Compound / ClipGroup / Blend / Warp /
    # Text / Image / Ref / Measurement) via a recursive tree walk.
    src/scripting/LayersScriptable.cpp
    src/scripting/GuidesScriptable.cpp
    src/scripting/SwatchesScriptable.cpp
    src/scripting/PalettesScriptable.cpp
    src/scripting/StylesScriptable.cpp
    src/scripting/ThemesScriptable.cpp
    src/scripting/ObjectsScriptable.cpp
    # s222 m2 — inspector area Scriptable. Flat verb surface (no proxy
    # routing) that delegates to MainWindow's collapse-all and
    # apply-section-open methods. Sibling of the model Scriptables but
    # a different shape — meta over the window's layout, not over a
    # project sub-system.
    src/scripting/InspectorScriptable.cpp
    # s246 m1 — `proj` Scriptable. First headless-verb singleton from
    # ARC m5b (Tier 4). Same singleton shape as inspector (flat verb
    # surface, no proxy routing, MainWindow-pointer ctor) but wraps
    # the project-level save outcome rather than the inspector area's
    # section orchestration. One verb (`save`) + two queries (`path`,
    # `has_path`) in m1; save_as / load / new / close land in s247+.
    src/scripting/ProjScriptable.cpp
    # s251 m1 — `export` Scriptable. Second headless-verb singleton from
    # ARC m5b (Tier 4 row 1/~4-5 → 2/~4-5). Sibling singleton-host
    # pattern as proj (flat verb surface, no proxy routing, MainWindow-
    # pointer ctor) but wraps the per-format writers rather than the
    # project lifecycle. One verb (`svg <path>`) + two queries
    # (`last_path`, `last_ok`) in m1; png / ico / refpt / gresource
    # land in m2+ as the surface widens. Cardinality is by concern,
    # not by format — one Scriptable, five format-verbs.
    src/scripting/ExportScriptable.cpp
    src/curvz/widgets/ToggleButton.cpp
    src/curvz/widgets/Button.cpp
    src/curvz/widgets/CheckButton.cpp
    src/curvz/widgets/DropDown.cpp
    src/curvz/widgets/Entry.cpp
    src/curvz/widgets/Scale.cpp
    src/curvz/widgets/SpinButton.cpp
    src/curvz/widgets/RefPointPicker.cpp  # s204 m4 — compound bbox-point picker

    # Custom widgets
    src/CurvzSwitch.cpp

    # Unit system
    src/CurvzSpinButton.cpp

    # Unit system
    src/UnitSystem.cpp

    # s177: canonical doc->display coordinate pump (used by
    # PropertiesPanel and RefptExporter so the export coordinates
    # match exactly what the inspector and ruler show).
    src/DocUnits.cpp

    # Print manager
    src/PrintManager.cpp

    # GLib compiled resources (generated at build time into src/)
    "${RESOURCES_C}"
)

# Force the generated resource file to compile as C, not C++
set_source_files_properties("${RESOURCES_C}" PROPERTIES LANGUAGE C)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(curvz ${SOURCES})

# Ensure resources are generated before any source file is compiled
add_dependencies(curvz curvz_resources_gen)

target_include_directories(curvz PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GTKMM_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${CAIROMM_INCLUDE_DIRS}
    ${PANGOMM_INCLUDE_DIRS}
    ${PANGOCAIRO_INCLUDE_DIRS}
    ${PANGOFT2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
)

target_link_libraries(curvz PRIVATE
    ${GTKMM_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${CAIROMM_LIBRARIES}
    ${PANGOMM_LIBRARIES}
    ${PANGOCAIRO_LIBRARIES}
    ${PANGOFT2_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${PNG_LIBRARIES}
    ${GIO_LIBRARIES}
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Clipper2
    ${CURVZ_CMARK_TARGET}
    fmt
)

target_compile_options(curvz PRIVATE
    ${GTKMM_CFLAGS_OTHER}
    ${CAIRO_CFLAGS_OTHER}
)

# Enable all spdlog levels (DEBUG and above) and source file/line in log output.
target_compile_definitions(curvz PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
)

# s219 m1 — the CURVZ_DIAGNOSTIC sources block is gone. The previously-
# gated TUs (ScriptListener.cpp, ScripterWindow.cpp, StylesPanelScriptable.cpp,
# LayersScriptable.cpp, GuidesScriptable.cpp) are folded into the main
# SOURCES list above and compile unconditionally. Runtime visibility is
# governed by the AppPreferences::scripter_enabled preference, wired to
# a Developer menu and an Application ▸ Developer inspector toggle.

# ── Install ───────────────────────────────────────────────────────────────────
include(GNUInstallDirs)

# Main binary.
install(TARGETS curvz DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop entry — substitute @CURVZ_APP_ID@ from the .in template, then install
# the result as <app-id>.desktop into share/applications.
configure_file(
    ${CMAKE_SOURCE_DIR}/resources/freedesktop/${CURVZ_APP_ID}.desktop.in
    ${CMAKE_BINARY_DIR}/${CURVZ_APP_ID}.desktop
    @ONLY
)
install(
    FILES ${CMAKE_BINARY_DIR}/${CURVZ_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# AppStream metainfo — required for Flathub submission, surfaced by
# software centres (GNOME Software, KDE Discover) for description and
# screenshots.
install(
    FILES ${CMAKE_SOURCE_DIR}/resources/freedesktop/${CURVZ_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
)

# MIME type — registers .curvz files so file managers double-click into
# Curvz and show the correct icon. Filename must match the namespace
# convention (no app-id prefix is required, but keeping it is harmless and
# avoids collisions).
install(
    FILES ${CMAKE_SOURCE_DIR}/resources/freedesktop/${CURVZ_APP_ID}.mime.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages
)

# Application icon — the dock looks here for an SVG matching the app ID.
# Source is curvz-app-icon.svg (full-colour launcher icon, distinct from the
# in-app symbolic). Renamed at install time so the basename matches
# CURVZ_APP_ID exactly. Without this, GNOME's dash falls back to a generic
# placeholder icon.
install(
    FILES ${CMAKE_SOURCE_DIR}/resources/branding/curvz-app-icon.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME ${CURVZ_APP_ID}.svg
)

# ── Post-install hooks ────────────────────────────────────────────────────────
# When installing into a real prefix (not staging for a package), refresh the
# desktop, MIME, and icon caches so changes take effect immediately. Distro
# package managers (rpm, dpkg, flatpak-builder) handle this themselves, so we
# skip when DESTDIR is set or when CMAKE_INSTALL_PREFIX points outside /usr.
if(NOT DEFINED ENV{DESTDIR})
    install(CODE "
        message(STATUS \"Updating desktop database, MIME database, and icon cache...\")
        execute_process(COMMAND update-desktop-database -q
            \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications
            ERROR_QUIET RESULT_VARIABLE _r)
        execute_process(COMMAND update-mime-database -n
            \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/mime
            ERROR_QUIET RESULT_VARIABLE _r)
        execute_process(COMMAND gtk-update-icon-cache -qtf
            \${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor
            ERROR_QUIET RESULT_VARIABLE _r)
    ")
endif()
