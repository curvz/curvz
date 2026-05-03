#pragma once
//
// Palette.hpp — an ordered collection of Swatch references.
//
// Per ARCHITECTURE.md "Color System":
//   A Palette is an ordered vector of SwatchIds, with a name, description,
//   source, and builtin flag. Curated built-in palettes (Earth Tones, etc.)
//   ship as read-only with builtin=true; the UI enforces duplicate-to-edit.
//
// Phase 1 / M4: type definition only. Storage and CRUD are on SwatchLibrary;
// v1 ships no built-in palettes and provides no UI for palettes (Phase 4).
//

#include "color/Paint.hpp"  // SwatchId

#include <string>
#include <vector>

namespace Curvz {
namespace color {

using PaletteId = std::string;

struct Palette {
    // Project-scoped unique id. Stable across renames.
    PaletteId id;

    // Display name, user-editable.
    std::string name;

    // Free-form description; may render as a subtitle in the picker.
    std::string description;

    // Provenance: where this palette came from. Examples: "user", "imported:
    // warmgreys.curvzpal", "builtin:earth-tones". Not parsed — treat as a
    // label for UI display. Phase 4 decides the canonical string set.
    std::string source;

    // Read-only when true. Built-in curated palettes set this. The UI must
    // enforce duplicate-to-edit; direct mutation of a builtin=true palette
    // is a bug.
    bool builtin = false;

    // Ordered swatch references. Order is user-visible and persisted —
    // palettes carry a designer's intended reading order, not just a set.
    // SwatchIds must resolve within the containing SwatchLibrary; dangling
    // ids are a library-integrity failure, not a palette-level concern.
    std::vector<SwatchId> swatches;
};

} // namespace color
} // namespace Curvz
