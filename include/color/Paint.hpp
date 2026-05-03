#pragma once
//
// Paint.hpp — the "what does an object's fill/stroke hold" variant.
//
// Design notes (see ARCHITECTURE.md "Color System → Paint as variant"):
//
//   Paint is a std::variant, not a tagged struct with optional fields. This
//   replaces the existing FillStyle (which has an enum Type plus RGBA that
//   are logically undefined for None/CurrentColor). The variant makes the
//   "there is no color here" case a separate type, not a convention, and
//   lets std::visit enforce exhaustiveness at compile time.
//
//   Five variants:
//     None          — the SVG "fill=none" case. No color data.
//     CurrentColor  — SVG "currentColor". Defers to containing context.
//     Solid         — a literal Color (including alpha).
//     SwatchRef     — a reference to a Swatch in the SwatchLibrary.
//     Gradient      — linear or radial gradient with stops + geometry.
//                     (S93 m1: added to unblock Style Manager gradient
//                     editing — see color/Gradient.hpp.)
//
//   Phase 5 makes SwatchRef live: the swatch panel writes refs, the reverse
//   usage index in SwatchLibrary tracks sites, and live recolour redraws
//   every object bound to a swatch when the swatch is edited. SVG round-trip
//   lands in a later Phase 5 milestone.
//
//   SwatchRef carries a cached `fallback` colour alongside the id. This is
//   the resolved colour at the time of the last set_paint() call, kept in
//   lockstep with swatch edits. Two reasons it exists:
//
//     1. SVG export is always safe — the writer emits `fallback` as the
//        concrete hex (alongside a data-curvz-swatch-id attribute for
//        Curvz-aware readers). No lookup, no lifetime trap.
//
//     2. Dead refs (swatch deleted without convert-to-solid) degrade to a
//        visible colour instead of disappearing. resolve_paint() falls back
//        to the cached value if the library doesn't know the id.
//
//   The fallback is NOT part of identity: two SwatchRefs with the same id
//   compare equal regardless of their cached colours. That keeps equality
//   stable across live recolours.
//
// Overloaded helper:
//   The canonical two-line trick for using std::visit with a set of
//   per-variant lambdas. Let renderers dispatch like:
//
//     std::visit(Overloaded{
//         [&](const None&)         { ... fill="none" ... },
//         [&](const CurrentColor&) { ... fill="currentColor" ... },
//         [&](const Solid& s)      { ... use s.color ... },
//         [&](const SwatchRef&)    { ... resolve later ... },
//         [&](const Gradient& g)   { ... emit linearGradient/radialGradient ... },
//     }, paint);
//
//   The compiler flags every visit site that doesn't handle all five cases.
//

#include "color/Color.hpp"
#include "color/Gradient.hpp"

#include <string>
#include <variant>

namespace Curvz {
namespace color {

// SwatchId is a project-scoped handle. v1 uses a string for simplicity and
// SVG round-trip (the hex-with-sidecar-id writer, Phase 9, will emit it as
// data-curvz-swatch-id="..."). If profiling later shows string comparison
// hot in the reverse-usage index lookup, switch to an integer handle — the
// using declaration keeps the migration local.
using SwatchId = std::string;

struct None {};
struct CurrentColor {};

struct Solid {
    Color color;

    Solid() = default;
    explicit Solid(const Color& c) : color(c) {}
};

struct SwatchRef {
    SwatchId id;

    // Cached resolved colour. Refreshed on every set_paint() call and on
    // live recolour when the referenced swatch is edited. Used by the SVG
    // writer as the concrete hex and by resolve_paint() when the library
    // doesn't know `id` (dead-ref graceful degradation).
    //
    // Not part of identity — see operator== below.
    Color fallback{};

    SwatchRef() = default;
    explicit SwatchRef(SwatchId id) : id(std::move(id)) {}
    SwatchRef(SwatchId id, Color fallback)
        : id(std::move(id)), fallback(fallback) {}
};

using Paint = std::variant<None, CurrentColor, Solid, SwatchRef, Gradient>;

// Overloaded helper for std::visit. Inherits call operators from each
// lambda so a brace-enclosed list produces a single callable handling
// every variant. C++17 pattern; the deduction guide (second line) lets
// us write `Overloaded{lambda1, lambda2, ...}` without spelling out the
// types.
template <class... Fs> struct Overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;

// --- Equality --------------------------------------------------------------
//
// None == None, CurrentColor == CurrentColor (type-only, no payload).
// Solid compares colors at 8-bit granularity (per Color's operator==).
// SwatchRef compares ids only — the cached `fallback` colour is a resolve
// cache, not identity. Two refs to the same swatch are equal even if one
// has a stale cache.
// Cross-variant comparisons are always false.
//
// Declared as free functions so they're overridable / ADL-findable if we
// ever need custom semantics; std::variant's default operator== would work
// too, but being explicit documents the intent.

inline bool operator==(const None&,         const None&)         { return true; }
inline bool operator==(const CurrentColor&, const CurrentColor&) { return true; }
inline bool operator==(const Solid& a,      const Solid& b)      { return a.color == b.color; }
inline bool operator==(const SwatchRef& a,  const SwatchRef& b)  { return a.id    == b.id; }

inline bool operator!=(const None& a,         const None& b)         { return !(a == b); }
inline bool operator!=(const CurrentColor& a, const CurrentColor& b) { return !(a == b); }
inline bool operator!=(const Solid& a,        const Solid& b)        { return !(a == b); }
inline bool operator!=(const SwatchRef& a,    const SwatchRef& b)    { return !(a == b); }

} // namespace color
} // namespace Curvz

// --- Resolution ------------------------------------------------------------
//
// resolve_paint lives in its own translation unit (Paint.cpp) because it
// depends on SwatchLibrary, which would pull nlohmann/json into every site
// that includes Paint.hpp if we inlined it. Forward-declare SwatchLibrary
// here and keep the header dependency-light.

namespace Curvz {
namespace color {

class SwatchLibrary;

// Resolve a Paint to a concrete Color. Used by renderers (via the interop
// bridge today, directly in later phases) and by SVG export.
//
//   None           -> transparent (a == 0)
//   CurrentColor   -> current_color argument (caller supplies the ambient
//                     colour, typically the containing element's text / fg
//                     colour). Defaults to opaque black if unspecified.
//   Solid          -> its own colour.
//   SwatchRef      -> library lookup on `id`, falling back to `fallback`
//                     if the library is null or the id is unknown.
//
// Pure function, no side effects. Safe to call from draw code.
Color resolve_paint(const Paint& p,
                    const SwatchLibrary* lib = nullptr,
                    const Color& current_color = Color::black());

} // namespace color
} // namespace Curvz

// --- JSON round-trip -------------------------------------------------------
//
// S76 m3b — canonical Paint serialiser. Lives here rather than in
// StyleInterop because SwatchRef SVG round-trip (future Phase 5 milestone)
// will want the same helpers; there should be exactly one Paint<->JSON
// representation in the codebase.
//
// Schema (shallow, "type" field discriminates):
//   {"type": "none"}
//   {"type": "currentcolor"}
//   {"type": "solid", "color": "#rrggbbaa"}
//   {"type": "swatchref", "id": "sw_...", "fallback": "#rrggbbaa"}
//
// Colour hex uses color::to_hex / from_hex (lowercase, alpha included when
// < 1.0). Mirrors the SwatchLibrary JSON style exactly: lowercase
// discriminant, .value(...) defaulting for missing fields.
//
// paint_from_json returns None{} on any malformed input and LOG_WARNs —
// consistent with SwatchLibrary::load_pool's tolerance for partial data.
//
// Uses nlohmann/json_fwd.hpp (forward-decl header) rather than the full
// json.hpp: honours the dependency-light constraint at the top of this
// file — resolve_paint lives in Paint.cpp specifically to avoid dragging
// nlohmann/json into every include site, and the JSON helpers follow the
// same discipline. Implementations in Paint.cpp pull the full header.

#include <nlohmann/json_fwd.hpp>

namespace Curvz {
namespace color {

nlohmann::json paint_to_json(const Paint& p);
Paint          paint_from_json(const nlohmann::json& j);

} // namespace color
} // namespace Curvz
