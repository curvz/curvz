#pragma once
//
// Style.hpp — a named, reusable bundle of fill paint + stroke appearance.
//
// Per ARCHITECTURE.md "Style System":
//   A Style is Illustrator's "Graphic Styles" panel adapted to Curvz idioms —
//   sibling to the colour system. Where Swatches give the user reusable
//   colour values, Styles give them reusable complete appearances.
//
// Phase 1 scope (this header):
//   Data model only. No CRUD, no library, no UI. Sees:
//     * StyleId            — project-scoped string handle.
//     * StyleHeader        — id/name/category metadata bundle.
//     * StrokeAppearance   — stroke description (paint + width + cap +
//                            join + miter limit). S87 m1: dash + dash_offset
//                            and stroke alignment removed — they had been
//                            specced for a future phase but never reached
//                            the renderer (StrokeAppearance had the fields,
//                            materialise_from_style explicitly dropped them
//                            when projecting onto SceneNode StrokeStyle).
//                            When real dash/align support lands, fields will
//                            be added to BOTH StrokeAppearance AND SceneNode
//                            StrokeStyle in lockstep.
//     * ShadowAppearance   — drop-shadow description (enabled + offset +
//                            blur + colour + opacity). S98: peer of
//                            StrokeAppearance, not collapsed into
//                            SceneNode's ShadowParams (which is a
//                            SceneNode-edit-undo transport, semantically
//                            distinct from the style-system canonical
//                            type even though the field set is currently
//                            identical — same precedent as StrokeAppearance
//                            vs StrokeStyle).
//     * Style              — header + Paint fill + StrokeAppearance +
//                            ShadowAppearance.
//     * is_built_in(id)    — predicate, true iff id starts with "app:".
//
// Identity scheme (per addendum decision 3 — duplicate-to-customise):
//   * Built-in styles: id prefix "app:" + slug, e.g. "app:brand-outline".
//     Stable across machines, never user-edited, never serialised. The
//     library hardcodes them at construction.
//   * User styles:    id prefix "stl_" + UUID (g_uuid_string_random via
//     generate_internal_id()). Serialised with the project. Editable.
//   The is_built_in() predicate is the gate every CRUD path checks before
//   accepting a write. Mirrors the SwatchLibrary defaults-vs-custom split,
//   but without the file I/O layer (app styles are code, not config).
//
// Why not a variant like Swatch:
//   Swatch is std::variant<SolidSwatch /*, GradientSwatch, ... */> because
//   the Phase-4 design anticipates kind-specific recipe data (gradient
//   stops, blend curves) that Solid doesn't have. Style has no such
//   forking — every style has a fill (a Paint, which is itself the variant)
//   and a stroke (a StrokeAppearance, fixed shape). Adding a new "kind"
//   to Style would be inventing a problem; we keep the type a plain
//   struct.
//
// Why Paint and not FillStyle:
//   The Style System is built on the new variant Paint model from the
//   colour system Phase 5 work. Paint can hold None / CurrentColor /
//   Solid / SwatchRef directly, so a style fill that's a swatch reference
//   costs no extra plumbing — a swatch edit ripples through bound styles
//   to bound objects via the existing SwatchLibrary signal machinery.
//   The bridge to SceneNode's older FillStyle / StrokeStyle is the
//   materialise_from_style helper in StyleInterop (Phase 1 m3), which
//   uses FillStyleInterop's existing Paint→FillStyle conversion.
//

#include "color/Paint.hpp"
#include "SceneNode.hpp"  // LineCap, LineJoin (defined in Curvz namespace)

#include <string>

namespace Curvz {
namespace style {

// Project-scoped handle. v1 uses a string for SVG round-trip ergonomics
// (data-curvz-style="..."). The "app:" / "stl_" prefix discrimination is
// the only structure imposed on the value; everything else is opaque.
using StyleId = std::string;

// True iff the id refers to an app (built-in) style. The addendum's
// canonical "duplicate-to-customise" UX gate: every CRUD path consults
// this before accepting a write. Implemented inline because every site
// that touches a StyleId is a candidate caller; no point making it a
// translation-unit-bound function.
inline bool is_built_in(const StyleId& id) {
    // Cheap prefix check, no normalisation. App ids are exactly
    // "app:<slug>"; we don't try to be clever about whitespace or case.
    static constexpr char prefix[] = "app:";
    static constexpr std::size_t prefix_len = sizeof(prefix) - 1;
    return id.size() >= prefix_len &&
           id.compare(0, prefix_len, prefix) == 0;
}

// Metadata shared by every style. Lives at the head of each Style and is
// the only part of a style the library iterates by name / category. The
// flat single-string `category` is the addendum's decision 4 — no nested
// folders. UI groups by exact-string equality.
struct StyleHeader {
    // Project-scoped unique id. "app:<slug>" for built-in, "stl_<uuid>"
    // for user. is_built_in() is the discriminant.
    StyleId id;

    // Display name, user-editable for user styles. App styles fix this
    // at construction; renames don't apply. UI falls back to id when
    // empty (defensive — should never happen for either tier).
    std::string name;

    // Section header in the panel. Empty string means "uncategorised" —
    // a panel-level convention, not enforced here. Equality is exact
    // (case-sensitive, full string); no normalisation.
    std::string category;
};

// Full stroke description. Mirrors what SceneNode::StrokeStyle carries
// (paint + width + cap + join + miter limit) — kept as a separate type
// because Style is the canonical source of truth in the Style System
// even though the field set is currently identical.
//
// Defaults match the SVG default conventions: black fill, width-1
// butt-end miter-join stroke. A freshly-default-constructed Style is
// a sensible "empty" appearance.
struct StrokeAppearance {
    // Paint variant — None / CurrentColor / Solid / SwatchRef. Same
    // model as fill; the Style System is paint-source agnostic.
    color::Paint paint;

    // Stroke width in user units (document space). 1.0 by default to
    // match SVG's default. 0.0 is meaningful — it's the "no stroke"
    // visual equivalent without forcing paint to None, but None is
    // preferred for that case (renderer can short-circuit).
    double width = 1.0;

    LineCap  cap  = LineCap::Butt;
    LineJoin join = LineJoin::Miter;

    // Miter limit. Same units / semantics as SVG stroke-miterlimit.
    // Ignored when join != Miter. Field is honoured by the renderer
    // (Cairo set_miter_limit) but no UI surfaces an editor for it as
    // of S87 — JSON-edited styles preserve their values, and styles
    // edited through the StyleEditorDialog get the StrokeAppearance
    // default of 4.0 (matches Cairo's own default).
    double miter_limit = 4.0;
};

// Full shadow description. Mirrors what SceneNode carries as loose
// shadow_* fields (and bundles as ShadowParams for undo transport) —
// kept as a separate type because Style is the canonical source of
// truth in the Style System even though the field set is currently
// identical. Same precedent as StrokeAppearance vs StrokeStyle.
//
// Defaults match SceneNode's shadow field defaults (S97 m4 icon-canvas
// tune): enabled=false, dy=0.5, blur=0.5, opaque-black colour at 50%
// opacity. A freshly-default-constructed Style has shadow disabled —
// binding to such a style turns shadow off on the host node, which is
// the right semantic (the style is the appearance, full stop).
//
// Y-down convention for dy (matches SceneNode and SVG <feOffset>):
// positive dy is "below". This is the doc-space convention; the
// inspector / toolbar SpinType layer handles any user-facing flip.
struct ShadowAppearance {
    // Gate. False = no shadow at render. Default off so binding a
    // freshly-created style doesn't surprise users with a shadow they
    // didn't pick.
    bool   enabled = false;

    // Offset from the host, in document units. dy positive = below.
    double dx      = 0.0;
    double dy      = 0.5;

    // Gaussian blur radius (one stddev) in doc units. 0.0 = sharp
    // offset. Renderer clamps to >= 0.
    double blur    = 0.5;

    // Tint, premultiplied with `opacity` at render. Defaults to opaque
    // black; alpha is the colour-side multiplier, `opacity` below is
    // a separate user-facing dial that lets a saturated tint be dimmed
    // without re-picking.
    double color_r = 0.0;
    double color_g = 0.0;
    double color_b = 0.0;
    double color_a = 1.0;

    // Final alpha multiplier on the shadow, after the colour's own
    // alpha. Range 0..1. Matches SceneNode::shadow_opacity.
    double opacity = 0.5;
};

// The Style itself. Header + fill paint + stroke appearance + shadow
// appearance. No variant, no kind discriminator — every style has the
// same shape (see top-of-file rationale).
struct Style {
    StyleHeader      header;
    color::Paint     fill;    // Defaults to None (Paint's first variant).
    StrokeAppearance stroke;
    ShadowAppearance shadow;  // S98: drop-shadow as a first-class
                              // appearance dimension. Default-disabled.
};

// ── Equality (S98) ──────────────────────────────────────────────────────────
//
// Style equality is field-wise across every member of every sub-struct.
// Every editable Style field that round-trips through the editor must
// participate, otherwise the StylesPanel's "skip unchanged commit"
// predicate silently drops edits to the omitted fields.
//
// Why defaulted operator==: C++17 doesn't synthesise it (that's C++20),
// so we write each one by hand. Each compares every member field — no
// elision, no "this field doesn't matter for equality" carve-outs. If
// you add a field to any of these structs and don't add it here, you
// reintroduce the S98 m2 bug where edits to the new field appear to
// not save (the panel's unchanged predicate matched and dropped the
// commit). One place to update; visible breakage if you forget.
//
// Why these live here and not in the panel: equality is a Style-system
// concept, not a panel concept. The panel is one consumer; future
// consumers (cross-doc sync, undo merge, project-load diff) all need
// the same notion.
inline bool operator==(const StyleHeader& a, const StyleHeader& b) {
    return a.id       == b.id
        && a.name     == b.name
        && a.category == b.category;
}
inline bool operator!=(const StyleHeader& a, const StyleHeader& b) { return !(a == b); }

inline bool operator==(const StrokeAppearance& a, const StrokeAppearance& b) {
    return a.paint       == b.paint
        && a.width       == b.width
        && a.cap         == b.cap
        && a.join        == b.join
        && a.miter_limit == b.miter_limit;
}
inline bool operator!=(const StrokeAppearance& a, const StrokeAppearance& b) { return !(a == b); }

inline bool operator==(const ShadowAppearance& a, const ShadowAppearance& b) {
    return a.enabled == b.enabled
        && a.dx      == b.dx
        && a.dy      == b.dy
        && a.blur    == b.blur
        && a.color_r == b.color_r
        && a.color_g == b.color_g
        && a.color_b == b.color_b
        && a.color_a == b.color_a
        && a.opacity == b.opacity;
}
inline bool operator!=(const ShadowAppearance& a, const ShadowAppearance& b) { return !(a == b); }

inline bool operator==(const Style& a, const Style& b) {
    return a.header == b.header
        && a.fill   == b.fill
        && a.stroke == b.stroke
        && a.shadow == b.shadow;
}
inline bool operator!=(const Style& a, const Style& b) { return !(a == b); }

} // namespace style
} // namespace Curvz
