#pragma once
//
// StyleIO.hpp — bridge between StyleLibrary (in-memory user tier) and
// the on-disk JSON style-pack file (S102 m1).
//
// Per S102 plan "import/export json file of styles":
//   The user wants to ship a curated set of styles between projects (or
//   between users) without dragging the whole project.json around. The
//   shipped file holds just the user tier as a self-contained JSON
//   envelope.
//
// Layering — same shape as PaletteIO:
//   format::            — file-level read/write of the envelope (no
//                         StyleLibrary dep). Lives in this module's
//                         .cpp; not exposed because the in-memory shape
//                         (style_to_json / style_from_json on a single
//                         Style) is already public on StyleLibrary.hpp,
//                         and the envelope is trivial enough to keep
//                         private.
//   style_io::          — bridge: StyleLibrary ↔ file. The five
//                         entry points here are everything the panel
//                         needs.
//   StylesPanel UI      — calls style_io only, never touches files
//                         directly.
//
// File envelope:
//   {
//     "version": 1,
//     "styles":  [ <style_to_json output>, ... ]
//   }
//
// The "styles" array shape is byte-identical to what to_user_json
// emits, so a power user can hand-edit, diff against project.json, or
// concatenate multiple exports. The "version" key is permissive on
// read (missing = treated as 1) and lets us evolve the envelope later
// without a flag day.
//
// Atomic-undo contract on import:
//   import_styles_into_library batches every successful add into a
//   single CompositeCommand. One Ctrl+Z undoes the entire import,
//   regardless of how many styles were in the file. This matches
//   Scott's preference (palette imports are also atomic at the
//   library-mutation level, and "rip the bandaid" undo for bulk
//   operations is the project-wide pattern).
//
// Collision handling:
//   * Incoming ids are ALWAYS regenerated as fresh stl_<uuid>.
//     Importing a file means becoming local; preserving foreign uuids
//     would cause spurious "matches" if a style with the same id
//     happens to exist in the project. The format pump's id is a
//     storage-shape constraint, not a wire-protocol constraint.
//   * Incoming names get " 2", " 3", ... suffix until unique against
//     the merged user-tier name set. Same disambiguation semantics as
//     palette import.
//   * "app:"-prefixed entries in the file are dropped on import (they
//     belong only to the hardcoded constructor list — defensive,
//     mirrors from_user_json).
//

#include "style/StyleLibrary.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Curvz {

// fwd — import takes history so the multi-add is a single undoable unit.
class CommandHistory;

namespace style {
namespace style_io {

// ── User styles directory ────────────────────────────────────────────
//
// Path to the directory the export dialog defaults to:
//     ~/.config/curvz/styles/
// Mirrors palette_io::user_palettes_dir's shape and lifecycle —
// directory is NOT created here; export creates-as-needed, enumeration
// tolerates absence.

std::string user_styles_dir();

// ── Enumeration ──────────────────────────────────────────────────────
//
// Returns the stems (filename without .json) of every *.json file
// directly under user_styles_dir(). Alphabetical order, empty result
// when the directory doesn't exist or can't be read. Used by the
// kebab "Load styles" submenu to populate user-pack entries
// dynamically — same idiom as palette_io::enumerate_user.

std::vector<std::string> enumerate_user();

// ── File-level read/write ────────────────────────────────────────────
//
// load_user reads ~/.config/curvz/styles/<stem>.json and returns the
// decoded styles list (already filtered through style_from_json — bad
// or built-in entries are dropped with a warn log). Returns nullopt
// on a missing file, parse error, or wrong envelope shape.
//
// load_path is the same shape but takes an absolute path, used by the
// Import… file dialog.
//
// write_path serialises the supplied styles list to the given absolute
// path. Caller is responsible for the .json suffix and for ensuring
// the parent directory exists. Returns false on I/O failure.

std::optional<std::vector<Style>> load_user(const std::string& stem);
std::optional<std::vector<Style>> load_path(const std::string& path);
bool write_path(const std::string& path, const std::vector<Style>& styles);

// ── Import (file-decoded styles → StyleLibrary, atomic undo) ─────────
//
// Adds each incoming style to lib's user tier under a fresh stl_<uuid>
// id, with names disambiguated against the existing user-tier name set.
// Every successful add is wrapped in an AddStyleCommand and pushed as
// a single CompositeCommand on `history` — one Ctrl+Z reverts the
// entire import.
//
// Returns the number of styles successfully imported. Zero is a
// legitimate outcome (empty file, or every entry was an "app:" leak)
// — caller can decide whether to log/warn.
//
// `history` may be null; in that case adds happen directly via the
// library and are not undoable. The panel always supplies a real
// CommandHistory so this branch only matters for hypothetical
// non-panel callers and tests.
std::size_t import_styles_into_library(StyleLibrary&             lib,
                                       CommandHistory*           history,
                                       const std::vector<Style>& incoming);

// ── Export (StyleLibrary user tier → file-shape styles) ──────────────
//
// Snapshot the entire user tier into a vector<Style>. Pure read of the
// library; the panel passes the result to write_path. Built-in styles
// are NOT included — user styles are user data, app styles are code.
std::vector<Style> snapshot_user_tier(const StyleLibrary& lib);

} // namespace style_io
} // namespace style
} // namespace Curvz
