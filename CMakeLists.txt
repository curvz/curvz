cmake_minimum_required(VERSION 3.20)

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

project(curvz VERSION 0.1.0 LANGUAGES C CXX)

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

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
    src/NewDocumentDialog.cpp
    src/OffsetPathDialog.cpp
    src/GradientDialog.cpp
    src/SaveAsTemplateDialog.cpp
    src/ManageTemplatesDialog.cpp
    src/ShortcutsDialog.cpp
    src/HelpWindow.cpp
    src/StepRepeatDialog.cpp
    src/BlendDialog.cpp
    src/WarpDialog.cpp
    # S103 m3: Themes dialog — apply / manage / import / export.
    # Self-managed lifecycle (new + close); see ThemesDialog.hpp for
    # the dual-pane (apply + library) UX rationale.
    src/ThemesDialog.cpp
    # S104 m1: Export Documents dialog — per-doc SVG/PNG export with
    # user-chosen size (px/mm/in/pt + DPI) and fit-side. Self-managed
    # lifecycle. Distinct from ExportDialog (icon-theme bundle exporter,
    # File → Export Icon Theme…).
    src/ExportDocsDialog.cpp
    src/MacroSystem.cpp
    src/MacroManagerWindow.cpp
    src/MacroEditorWindow.cpp
    src/Canvas.cpp
    src/DocumentGallery.cpp
    src/PropertiesPanel.cpp
    src/CurvzColorPicker.cpp
    src/ColorPickerRecents.cpp
    src/ColorPickerPopover.cpp
    src/PaintEditor.cpp
    src/StyleEditorDialog.cpp
    src/PreviewPanel.cpp
    src/ContextBar.cpp
    src/DocTabBar.cpp
    src/LibraryPanel.cpp
    src/LayersPanel.cpp
    src/SwatchesPanel.cpp
    src/StylesPanel.cpp
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
    src/ExportDialog.cpp
    src/PngExporter.cpp
    src/IcoExporter.cpp
    src/GResourceExporter.cpp

    # System icon scanner
    src/SystemIconScanner.cpp

    # Undo/redo
    src/CommandHistory.cpp

    # Logging
    src/CurvzLog.cpp

    # Utilities — small, composable static pump methods in curvz::utils.
    # SVG-id codec lives here (S96); future utilities accumulate alongside.
    src/curvz_utils.cpp

    # Custom widgets
    src/CurvzSwitch.cpp

    # Unit system
    src/CurvzSpinButton.cpp

    # Unit system
    src/UnitSystem.cpp

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
