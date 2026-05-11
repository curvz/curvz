cmake_minimum_required(VERSION 3.20)

# ── scriptproto ──────────────────────────────────────────────────────────────
#
# Standalone sandbox proving the script-driven Curvz concepts before they
# land in the main app.
#
# s185 v1: hand-rolled wrappers, each inheriting (Gtk::X, Scriptable)
# directly. Proved the design end-to-end across seven widget types.
#
# s186 v2: introduces the ScriptableWidget<GtkBase> template base.
# Every leaf wrapper now derives from it instead of inheriting Scriptable
# directly. Centralises:
#   - name validation (non-empty, no whitespace, no DSL keyword)
#   - the registration call (Scriptable ctor) — leaves can't bypass
#   - canonical-signal binding (pure-virtual bind_canonical())
# Forgetting to wire emit is a compile error; forgetting init_scriptable()
# at the end of a leaf ctor is a debug-build assertion.
#
# Also adds lifetime_test.cpp — six checks (construct/destroy/recreate
# cycle + name-validation invariants) that run at startup before any
# window opens. Loud failure if anything is wrong.
#
# Build:
#   cmake -B build -S .
#   cmake --build build
#   ./build/scriptproto

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(scriptproto VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-psabi)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

set(SOURCES
    src/main.cpp
    src/lifetime_test.cpp
    src/ScripterWindow.cpp
    src/ScriptableRegistry.cpp
    src/ScriptListener.cpp
    src/widgets/ToggleButton.cpp
    src/widgets/Button.cpp
    src/widgets/Entry.cpp
    src/widgets/SpinButton.cpp
    src/widgets/Scale.cpp
    src/widgets/DropDown.cpp
    src/widgets/CheckButton.cpp
)

add_executable(scriptproto ${SOURCES})

target_include_directories(scriptproto PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${GTKMM_INCLUDE_DIRS}
)

target_link_libraries(scriptproto PRIVATE
    ${GTKMM_LIBRARIES}
)

target_compile_options(scriptproto PRIVATE
    ${GTKMM_CFLAGS_OTHER}
)
