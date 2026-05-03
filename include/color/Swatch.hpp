#pragma once
//
// Swatch.hpp — a reusable paint recipe with user-authored metadata.
//
// Per ARCHITECTURE.md "Color System":
//   A Swatch is a named, identifiable paint recipe. The simplest kind
//   (SolidSwatch) is just a Color; future kinds (GradientSwatch, and
//   possibly others) carry the parameters needed to reconstruct a more
//   complex fill at paint time. All kinds share a SwatchHeader carrying
//   the id/name/notes/tags that identify the swatch in the library.
//
// Phase 4 M1 refactor:
//   Previously Swatch was a single struct with a Color field. This split
//   introduces a discriminated-union shape so future gradient phases can
//   add variant cases without disturbing the library CRUD API. Right now
//   only SolidSwatch exists; the variant has exactly one case, and every
//   std::visit site becomes compiler-enforced exhaustive for later phases.
//
// Design rationale — why variant now:
//   At Phase 1 there were zero consumers of the old Swatch type
//   (FillStyleInterop only touched SwatchRef, not Swatch itself). That
//   made Phase 4 the cheapest moment in the project's life to split the
//   type. Later additions — LinearGradient, PathBlend, whatever else —
//   are a new `struct` + a new case in the variant, and std::visit sites
//   fail to compile until updated. No silent miss of a new kind.
//
// Per discussion with Scott: the model is PostScript-inspired. A swatch
// is a recipe that produces paint when invoked, using the target object's
// geometry as input. Solid swatches are the trivial recipe; gradient
// swatches compute their fill procedurally from the target's bbox and
// zoom level. See ARCHITECTURE.md "Color System → Swatch recipes" (to be
// added in the gradient phase).
//

#include "color/Color.hpp"
#include "color/Paint.hpp"  // SwatchId

#include <string>
#include <variant>
#include <vector>

namespace Curvz {
namespace color {

// Metadata shared across every swatch recipe kind. Carried as a field on
// each variant case rather than inherited — the variant stays POD-shaped
// and std::visit dispatches on the whole case, not through a base class.
struct SwatchHeader {
    // Project-scoped unique identifier. Treated as stable — rename changes
    // `name`, never `id`. SVG round-trip will use this via
    // data-curvz-swatch-id (Phase 9).
    SwatchId id;

    // Display name, user-editable. May be empty — UI falls back to the
    // swatch's hex (for solid) or a kind-specific label (for gradients).
    // Uniqueness is NOT enforced by SwatchLibrary — ids are unique, names
    // are not.
    std::string name;

    // Free-form user notes. Importers may populate this; users edit from
    // the swatch rename/edit dialog. Not searched beyond substring.
    std::string notes;

    // Tags for filtering / grouping. Plain strings, case-insensitive
    // equality enforced by SwatchLibrary on insert. Users add/remove from
    // the swatch editor; importers may pre-populate.
    std::vector<std::string> tags;
};

// The trivial recipe: a single colour. Paint is `Solid{color}`. No
// geometry, no interpolation. Most swatches in most projects are this.
struct SolidSwatch {
    SwatchHeader header;
    Color        color;
};

// Future variant cases land here. Sketch of the shape they'll take:
//
//   struct GradientSwatch {
//       SwatchHeader header;
//       // Recipe: stops + angle + endpoint source (bbox-derived /
//       // shape-preset / custom path pair). See design discussion in
//       // ARCHITECTURE.md "Color System → Gradient recipes".
//       std::vector<ColorStop> stops;
//       double                 angle_degrees;
//       EndpointSource         endpoint_source;
//       // ... kind-specific fields ...
//   };
//
//   struct PathBlendSwatch { ... };
//
// Adding any of these is additive:
//   1. Define the struct.
//   2. Add the type to the Swatch variant alias below.
//   3. Every std::visit site fails to compile until it adds a case.
//   4. JSON round-trip adds a new "kind": "gradient" discriminator.
//
// No changes to library storage, CRUD API, or UI dispatch plumbing.

using Swatch = std::variant<SolidSwatch /* GradientSwatch, PathBlendSwatch, ... */>;

// --- Header extraction -----------------------------------------------------
//
// Common accessor pattern: given a variant Swatch, pull out its header.
// Every variant case carries a SwatchHeader at the same logical position;
// std::visit dispatches to the right one. Used by the library for id
// lookup, name display, and tag filtering without caring about kind.

inline const SwatchHeader& swatch_header(const Swatch& s) {
    return std::visit([](const auto& v) -> const SwatchHeader& {
        return v.header;
    }, s);
}

inline SwatchHeader& swatch_header(Swatch& s) {
    return std::visit([](auto& v) -> SwatchHeader& {
        return v.header;
    }, s);
}

// Convenience: pull the id out of any swatch variant in one call.
inline const SwatchId& swatch_id(const Swatch& s) {
    return swatch_header(s).id;
}

} // namespace color
} // namespace Curvz
