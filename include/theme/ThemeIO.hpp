#pragma once
//
// ThemeIO.hpp — bridge between ThemeLibrary (in-memory user tier) and
// the on-disk JSON theme-pack file (S103 m2).
//
// Per the S103 milestone plan: mirrors S102 m1's StyleIO architecture
// exactly. The user wants to ship a curated set of themes between
// projects (or between users) without dragging the whole project.json
// around. The shipped file holds just the user tier as a self-contained
// JSON envelope.
//
// Layering — same shape as StyleIO and PaletteIO:
//   format::            — file-level read/write of the envelope (no
//                         ThemeLibrary dep). Lives in this module's
//                         .cpp; not exposed because the in-memory shape
//                         (theme_to_json / theme_from_json on a single
//                         Theme) is already public on ThemeLibrary.hpp,
//                         and the envelope is trivial enough to keep
//                         private.
//   theme_io::          — bridge: ThemeLibrary ↔ file. The seven entry
//                         points here are everything the m3 dialog will
//                         need.
//   Theme dialog (m3)   — calls theme_io only, never touches files
//                         directly.
//
// File envelope:
//   {
//     "version": 1,
//     "themes":  [ <theme_to_json output>, ... ]
//   }
//
// The "themes" array shape is byte-identical to what to_user_json
// emits, so a power user can hand-edit, diff against project.json, or
// concatenate multiple exports. The "version" key is permissive on
// read (missing = treated as 1) and lets us evolve the envelope later
// without a flag day.
//
// Atomic-undo contract on import:
//   import_themes_into_library batches every successful add into a
//   single CompositeCommand. One Ctrl+Z undoes the entire import,
//   regardless of how many themes were in the file. Same shape as
//   palette and style imports.
//
// Why apply isn't here:
//   ThemeIO is library-side I/O. It knows how to get themes from disk
//   into the library and back; it does NOT know how to apply themes
//   to documents. That bridge (apply_theme_to_doc) lives on
//   theme/Theme.hpp because it's the doc-side concern. The m3 dialog
//   is the orchestrator that pulls themes out of the library
//   (or via theme_io::load_path for a one-off without saving) and
//   feeds them to apply_theme_to_doc on each target.
//
// Collision handling on import:
//   * Incoming ids are ALWAYS regenerated as fresh thm_<uuid>.
//     Importing a file means becoming local; preserving foreign uuids
//     would cause spurious "matches" if a theme with the same id
//     happens to exist in the project.
//   * Incoming names get " 2", " 3", ... suffix until unique against
//     the merged user-tier name set. Same disambiguation semantics as
//     style import.
//   * "app:"-prefixed entries in the file are dropped on import
//     (they belong only to a hardcoded constructor list — none in
//     v1 anyway, but defensive).
//

#include "theme/ThemeLibrary.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Curvz {

// fwd — import takes history so the multi-add is a single undoable unit.
class CommandHistory;

namespace theme {
namespace theme_io {

// ── User themes directory ────────────────────────────────────────────
//
// Path to the directory the export dialog defaults to:
//     ~/.config/curvz/themes/
// Mirrors style_io::user_styles_dir's shape and lifecycle —
// directory is NOT created here; export creates-as-needed, enumeration
// tolerates absence.

std::string user_themes_dir();

// ── Enumeration ──────────────────────────────────────────────────────
//
// Returns the stems (filename without .json) of every *.json file
// directly under user_themes_dir(). Alphabetical order, empty result
// when the directory doesn't exist or can't be read. The m3 dialog's
// kebab "Load themes" submenu uses this — same idiom as styles and
// palettes.

std::vector<std::string> enumerate_user();

// ── File-level read/write ────────────────────────────────────────────
//
// load_user reads ~/.config/curvz/themes/<stem>.json and returns the
// decoded themes list (already filtered through theme_from_json — bad
// or built-in entries are dropped with a warn log). Returns nullopt
// on a missing file, parse error, or wrong envelope shape.
//
// load_path is the same shape but takes an absolute path, used by the
// Import… file dialog.
//
// write_path serialises the supplied themes list to the given absolute
// path. Caller is responsible for the .json suffix and for ensuring
// the parent directory exists. Returns false on I/O failure.

std::optional<std::vector<Theme>> load_user(const std::string& stem);
std::optional<std::vector<Theme>> load_path(const std::string& path);
bool write_path(const std::string& path, const std::vector<Theme>& themes);

// ── Import (file-decoded themes → ThemeLibrary, atomic undo) ─────────
//
// Adds each incoming theme to lib's user tier under a fresh thm_<uuid>
// id, with names disambiguated against the existing user-tier name set.
// Every successful add is wrapped in an AddThemeCommand and pushed as
// a single CompositeCommand on `history` — one Ctrl+Z reverts the
// entire import.
//
// Returns the number of themes successfully imported. Zero is a
// legitimate outcome (empty file, or every entry was an "app:" leak)
// — caller can decide whether to log/warn.
//
// `history` may be null; in that case adds happen directly via the
// library and are not undoable. The dialog always supplies a real
// CommandHistory so this branch only matters for hypothetical
// non-UI callers and tests.
std::size_t import_themes_into_library(ThemeLibrary&             lib,
                                       CommandHistory*           history,
                                       const std::vector<Theme>& incoming);

// ── Export (ThemeLibrary user tier → file-shape themes) ──────────────
//
// Snapshot the entire user tier into a vector<Theme>. Pure read of the
// library; the dialog passes the result to write_path. Built-in themes
// (when they exist in some future version) are NOT included — user
// themes are user data, app themes are code.
std::vector<Theme> snapshot_user_tier(const ThemeLibrary& lib);

} // namespace theme_io
} // namespace theme
} // namespace Curvz
