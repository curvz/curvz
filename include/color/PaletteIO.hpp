#pragma once
//
// PaletteIO.hpp — bridge between GplFormat (file shape) and
// SwatchLibrary (in-memory palette + swatch model).
//
// Per S101 handoff "palette interchange feature":
//   m3 needs three load paths and one save path: load bundled (from
//   gresource), load user file (from disk), load via Import…, and
//   save via Export current… . All four go through this seam.
//
// Layering:
//   GplFormat  → byte-level parser/writer (no SwatchLibrary dep)
//   PaletteIO  → format-records ↔ library-records translation
//   UI         → calls PaletteIO, never GplFormat directly
//
// Auto-naming policy (per UX rule "no UUIDs in user UI"):
//   GPL entries with empty names get the hex string of their colour
//   as the swatch name on import. Hex is unambiguous, searchable, and
//   matches the handoff's "Unnamed imports → display as hex" rule.
//   ColorRegion-derived names are richer but overkill for the import
//   path; hex is the contract here.
//
// On export, swatches whose name happens to match their hex round-trip
// as named-with-hex (we don't strip the name on output). A foreign
// reader (GIMP) sees the hex as the name and renders it that way too.
//

#include "color/GplFormat.hpp"
#include "color/Palette.hpp"     // PaletteId
#include "color/SwatchLibrary.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Curvz {
namespace color {

namespace palette_io {

// ── Bundled palette enumeration ──────────────────────────────────────
//
// The bundled palettes live as gresources under /com/curvz/app/palettes.
// enumerate_bundled() lists their stem names (filename without .gpl),
// in the order glib-compile-resources emits them — alphabetic by file
// name, which happens to match the handoff's curated order well enough.
// load_bundled(stem) reads + parses the resource and returns the
// GplPalette, or std::nullopt on a missing / malformed resource.
//
// Both functions are safe to call before any project is loaded; they
// touch only the static gresource bundle.

std::vector<std::string> enumerate_bundled();

std::optional<GplPalette> load_bundled(const std::string& stem);

// ── User palette enumeration ─────────────────────────────────────────
//
// User palettes live in ~/.config/curvz/palettes/*.gpl — the directory
// the export dialog defaults to (m5+). Each .gpl file is one palette;
// the stem (filename without extension) is the lookup key, matching
// bundled semantics.
//
// enumerate_user() returns the list of stems in alphabetical order.
// Returns empty if the directory doesn't exist (not an error — fresh
// installs haven't exported anything yet) or can't be read. load_user
// reads the file at the corresponding path and parses it.
//
// Stem collisions with bundled names are NOT prevented at this layer
// — UI display logic decides how to render them (e.g. show user
// palettes after bundled in the menu so a user "vivid" reads as a
// later override). The format module just enumerates what's there.

std::vector<std::string> enumerate_user();

std::optional<GplPalette> load_user(const std::string& stem);

// Path to the user palettes directory. Exposed for the file dialog's
// initial-folder hint and for any tooling that wants to introspect.
// The directory is NOT created by this function — read sites tolerate
// its absence; write sites (export) create-as-needed.
std::string user_palettes_dir();

// ── Import (GplPalette → SwatchLibrary) ──────────────────────────────
//
// import_gpl_into_library creates a NEW custom-tier palette in the
// library, populates it with one SolidSwatch per GplEntry, and returns
// the new palette's id. The palette's display name comes from the
// supplied `palette_name` argument (typically the GplPalette::name, or
// the file's basename if name was empty). Empty `palette_name` is
// rejected (no nameless palettes — UX rule), returns empty PaletteId.
//
// Append semantics (per handoff "append to the current swatch set, not
// replace"): nothing in the library is removed or modified. The new
// palette is independent. The caller decides whether to set it active
// after this call returns.
//
// On a name collision with an existing palette, the new palette gets
// " 2", " 3", … appended until unique. The caller does not need to
// pre-disambiguate.
//
// Auto-naming: GplEntries with empty `name` produce swatches whose
// name is `to_hex(color)`. Non-empty entry names are preserved
// verbatim.
//
// Failure modes:
//   - empty palette_name        → returns empty
//   - SwatchLibrary nullptr     → returns empty (with warn log)
//   - add_swatch / add_palette  → individual entries dropped on
//                                 failure with a warn log, but the
//                                 palette is still created with the
//                                 entries that did succeed.
PaletteId import_gpl_into_library(SwatchLibrary& lib,
                                  const GplPalette&  src,
                                  const std::string& palette_name);

// ── Export (SwatchLibrary palette → GplPalette) ──────────────────────
//
// Build a GplPalette by reading the named palette from the library.
// Resolves each SwatchId via lib.find_swatch (cross-tier — defaults
// and custom both visible). Non-solid swatch kinds are skipped with a
// warn log; GPL is RGB-only and gradient swatches don't fit. Alpha is
// dropped (GPL has no alpha channel; future extension `# Curvz: alpha=N`
// comments stay backward-compatible per the format-layer notes).
//
// The GplPalette's `name` field is set from the library palette's
// display name; `columns` is set to a sensible default (8) since the
// library's Palette type doesn't carry a column hint today.
//
// Returns std::nullopt if the palette id is unknown or has zero
// resolvable solid swatches (an empty-after-filtering palette is not
// useful to export, and probably indicates the user picked the wrong
// item).
std::optional<GplPalette> export_palette_as_gpl(const SwatchLibrary& lib,
                                                const PaletteId&     pid);

} // namespace palette_io
} // namespace color
} // namespace Curvz
