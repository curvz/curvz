#pragma once
//
// TextStyle.hpp — a named, inheritable paragraph text style.
//
// Per text_formatting_design.md §6/§7 (the named-paragraph-style layer of the
// rich-text sprint). This is the third layer of the text model: layer 1 is the
// per-run character spine (text_attr_spans on the buffer), layer 2 is paragraph
// format as fitting input, layer 3 is THIS — named styles like LibreOffice /
// macOS, flat AND parent-chain inheritable.
//
// The design is explicit that a paragraph-style system is ANOTHER INSTANCE of
// the existing style::StyleLibrary pattern (the graphic-styles library that is a
// sibling of swatches and themes on CurvzProject) — same two-tier shell (app:
// built-ins re-seeded every launch + stl_-style user entries serialised with the
// project), same CRUD/signals/JSON discipline, same swatch naming rules (non-
// empty names, UUIDs hidden). What is genuinely NEW here, and the only thing the
// graphic-styles library lacks:
//   * The bundle contents — a paragraph-format half + a character-default half,
//     instead of fill/stroke/shadow.
//   * Inheritance — a parent_id on the header and a resolve-up-the-chain pump.
//     A style stores DELTAS from its parent; editing the root (Default/Body)
//     cascades to every descendant that did not override the changed attribute.
//
// ── The delta model (design §7) ──────────────────────────────────────────────
//
// A stored style holds only what it OVERRIDES relative to its parent. So every
// field is std::optional<T>: set == "this style pins this value", unset ==
// "inherit from parent". The root (Default/Body) is effectively full because it
// pins everything; descendants are sparse. resolve() walks the parent chain
// accumulating set-fields and bottoms out at a hardcoded floor (so a resolve is
// total even if the root is somehow under-specified).
//
// This is NOT the per-run local override (the macOS "+" delta). That delta lives
// in the per-attribute spans on the buffer (text_attr_spans) and layers on top
// of the resolved style at the consumption seam — design §7's three-tier resolve
// is "style chain -> style -> local span". This file is the first two tiers; the
// third is the existing span machinery and meets this layer only at the fitter.
//
// ── Scope of THIS milestone (s340 m1) ────────────────────────────────────────
//
// Data model + library shell + the resolve pump, as a PURE ADDITION. Nothing
// consumes it yet:
//   * No CurvzProject sibling member, no project.json round-trip wiring (the
//     types carry to_json/from_json; the project hookup is a later milestone).
//   * No fitter consumption — ResolvedTextStyle is the form the fitter WILL read,
//     but no draw/layout path reads it yet.
//   * No SceneNode style-reference field (the run/paragraph data-curvz-style ref
//     of design §8) — a later milestone, mirroring SceneNode::bound_style.
//   * No UI (style bar / picker / verbs) — a later milestone.
//
// ── What is grounded vs deferred in the bundle ───────────────────────────────
//
// Every field below has an EXISTING runtime representation, so the eventual
// stamp-into-spans / resolve-from-node work is mechanical and verifiable against
// real data:
//   * align        <- kCurvzAlignAttr span ivalue (0=L 1=C 2=R 3=J) / text_align.
//   * leading      <- text_line_height scalar / kCurvzLeadingAttr span (doc-px).
//   * indent L/R/F <- the three kCurvzIndent* spans (doc-px).
//   * tabs         <- kCurvzTabsAttr span svalue (canonical "pos,type;..." grammar).
//   * char defaults<- text_font_family / text_font_size / text_bold / text_italic
//                     / text_letter_spacing scalars; colour as a Paint.
//
// DEFERRED (design §5/§6 — "Default owns the global justify-fit + hyphenation
// behaviour as real style data"): the InDesign-style justification spec (three
// lever {min,desired,max} triples + allocation policy + caps) and hyphenation.
// Their only consumer is the layer-2 justify engine, which is not built. Rather
// than bake a speculative serialised spec before its reader exists, the seam for
// it is marked in ParaFormat below; the fields slot in additively when the
// fitter that reads them lands (ParaFormat is optional-valued, so resolve already
// tolerates their absence).
//

#include "color/Paint.hpp"

#include <optional>
#include <string>

namespace Curvz {
namespace style {

// Project-scoped handle, same scheme as the graphic-style StyleId: "app:<slug>"
// for built-ins (stable across machines, never serialised, re-seeded every
// launch) and "txs_<uuid>" for user styles (serialised with the project). The
// "txs_" prefix (text-style) keeps the SVG/JSON round-trip able to spot a text-
// style ref at a glance and keeps it distinct from the graphic library's "stl_".
using TextStyleId = std::string;

// True iff the id refers to an app (built-in) text style. Same "app:" prefix
// gate as the graphic-style is_built_in — every CRUD path consults it before a
// write. Inline for the same reason: every site touching an id is a candidate
// caller.
inline bool is_built_in_text(const TextStyleId& id) {
    static constexpr char prefix[] = "app:";
    static constexpr std::size_t prefix_len = sizeof(prefix) - 1;
    return id.size() >= prefix_len &&
           id.compare(0, prefix_len, prefix) == 0;
}

// Paragraph alignment. Mirrors the kCurvzAlignAttr span ivalue (0=L 1=C 2=R
// 3=J) so the stamp/resolve at the consumption seam is a direct int cast.
// Justify is the one mode that feeds back into line-breaking (design §5); it is
// in the enum from day one even though the justify engine that honours it is a
// later layer — the alignment chip and the style data both need to name it now.
enum class ParaAlign { Left = 0, Center = 1, Right = 2, Justify = 3 };

inline int    para_align_to_ivalue(ParaAlign a) { return static_cast<int>(a); }
inline ParaAlign para_align_from_ivalue(int v) {
    switch (v) {
        case 1:  return ParaAlign::Center;
        case 2:  return ParaAlign::Right;
        case 3:  return ParaAlign::Justify;
        case 0:
        default: return ParaAlign::Left;
    }
}
inline const char* para_align_to_str(ParaAlign a) {
    switch (a) {
        case ParaAlign::Center:  return "center";
        case ParaAlign::Right:   return "right";
        case ParaAlign::Justify: return "justify";
        case ParaAlign::Left:
        default:                 return "left";
    }
}
inline ParaAlign para_align_from_str(const std::string& s) {
    if (s == "center")  return ParaAlign::Center;
    if (s == "right")   return ParaAlign::Right;
    if (s == "justify") return ParaAlign::Justify;
    return ParaAlign::Left;
}

// Metadata shared by every text style. Same shape as the graphic StyleHeader
// (id / name / category) PLUS the one new field that makes inheritance possible:
// parent_id. Single parent; the root (Default/Body) carries an empty parent_id.
struct TextStyleHeader {
    // Project-scoped unique id. "app:<slug>" built-in, "txs_<uuid>" user.
    TextStyleId id;

    // Display name. User-editable for user styles; app styles fix it at
    // construction. UI falls back to id when empty (defensive).
    std::string name;

    // Flat single-string section header in the panel. Empty == uncategorised.
    // Exact-string equality, no normalisation — same convention as StyleHeader.
    std::string category;

    // Single-parent inheritance link. Empty == this is a root (the Default/Body
    // style and only it, in the seeded set). A style stores deltas FROM this
    // parent; resolve() walks up the chain. A dangling parent_id (parent not in
    // the library) resolves as "stop here" — the chain bottoms at the hardcoded
    // floor, which is the safe degenerate.
    TextStyleId parent_id;
};

// The paragraph-format half of the bundle. Sparse: every field optional so a
// non-root style stores only its deltas. Units chosen to match the existing
// runtime spans so stamp/resolve is mechanical (see top-of-file grounding note).
struct ParaFormat {
    // Paragraph alignment. Unset -> inherit. (span: kCurvzAlignAttr ivalue)
    std::optional<ParaAlign> align;

    // Line stride (leading) in DOCUMENT PIXELS. 0.0 is a meaningful explicit
    // value distinct from unset: 0.0 == "derive from font metrics" (the tier-3
    // metric default), a set non-zero == an explicit stride. Unset (nullopt) ==
    // inherit the parent's leading. (span: kCurvzLeadingAttr ivalue / doc-px;
    // node scalar: text_line_height.)
    //
    // Tier note (handoff s339 leading finding): the old buffer-global scalar
    // (text_line_height) is the "tier 2" duplicate-truth the cross-box mismatch
    // exposed. Its natural home in this model is the Default style's leading —
    // i.e. the cascade floor owns it. This milestone does not perform that
    // migration (text is freshly stable; the replace-vs-alongside fork stays
    // parked), but the leading field here is the destination when it happens.
    std::optional<double> leading_px;

    // Indents in DOCUMENT PIXELS. first applies only to a paragraph's first
    // visual line (a hard boundary, not a soft-wrap continuation), matching the
    // kCurvzIndentFirstAttr semantics. Unset -> inherit.
    std::optional<double> indent_left_px;
    std::optional<double> indent_right_px;
    std::optional<double> indent_first_px;

    // Tab stops, stored as the canonical kCurvzTabsAttr svalue grammar
    // ("pos,type;pos,type;..." — pos doc-px, type one of L/R/C/D). Empty string
    // is a meaningful explicit value (no stops -> Pango default interval); unset
    // (nullopt) == inherit. Keeping the svalue string (rather than a structured
    // vector<TabStop>) makes the stamp into a span a verbatim copy, and the
    // existing tab codec in curvz_utils parses it at the consumption seam.
    std::optional<std::string> tabs;

    // ── DEFERRED seam (design §5/§6) ─────────────────────────────────────────
    // Justify-fit spec (word-spacing / tracking / glyph-scale {min,desired,max}
    // triples + allocation policy + caps) and hyphenation (enable + penalty)
    // land HERE when the layer-2 justify engine that consumes them is built.
    // They are intentionally absent now: their only reader doesn't exist yet,
    // and ParaFormat being optional-valued means resolve() already tolerates
    // their later addition with no migration. See top-of-file note.
};

// The character-default half of the bundle — the baseline style for runs in a
// paragraph carrying this style, before any per-run span override. Sparse, same
// inherit-on-unset contract as ParaFormat.
struct CharDefaults {
    // Font family (e.g. "Sans"). (node scalar: text_font_family)
    std::optional<std::string> family;

    // Font size in DOCUMENT UNITS. (node scalar: text_font_size)
    std::optional<double> size;

    // Weight / slant as the two booleans the text model currently carries.
    // (node scalars: text_bold / text_italic; spans: PANGO_ATTR_WEIGHT /
    // PANGO_ATTR_STYLE.) Booleans (not a full weight enum) because that is the
    // resolution the rest of the text model exposes today; widening to a weight
    // scale is an independent later change touching node + span + this in
    // lockstep.
    std::optional<bool> bold;
    std::optional<bool> italic;

    // Extra inter-glyph advance in DOCUMENT UNITS. (node scalar:
    // text_letter_spacing)
    std::optional<double> letter_spacing;

    // Base text colour as a Paint (None / CurrentColor / Solid / SwatchRef) —
    // same model as the graphic Style fill, so a swatch-ref colour ripples
    // through bound styles via the existing SwatchLibrary signal machinery. The
    // per-run colour override rides PANGO_ATTR_FOREGROUND spans at the layer
    // above; this is the paragraph baseline. Unset -> inherit.
    std::optional<color::Paint> colour;
};

// The text style itself: header + the two sparse halves. No variant, no kind
// discriminator — every text style has the same shape (same rationale as the
// graphic Style).
struct TextStyle {
    TextStyleHeader header;
    ParaFormat      para;
    CharDefaults    chars;
};

// The fully-cascaded result of walking a style's parent chain (and, at the
// consumption seam, layering local span overrides on top). Every field is
// CONCRETE — no optionals — because a resolve is total: it bottoms out at a
// hardcoded floor matching the SceneNode text defaults, so even an empty library
// or a dangling parent yields a usable style. This is the form the fitter /
// renderer reads; ParaFormat/CharDefaults (sparse) are the storage form.
struct ResolvedTextStyle {
    // Paragraph format (floor values match SceneNode text defaults).
    ParaAlign   align          = ParaAlign::Left;
    double      leading_px      = 0.0;   // 0 == derive from metrics (tier-3)
    double      indent_left_px  = 0.0;
    double      indent_right_px = 0.0;
    double      indent_first_px = 0.0;
    std::string tabs;                    // empty == Pango default interval

    // Character defaults (floor values match SceneNode text defaults).
    std::string  family         = "Sans";
    double       size           = 24.0;  // document units
    bool         bold           = false;
    bool         italic         = false;
    double       letter_spacing = 0.0;
    color::Paint colour         = color::Solid{color::Color::black()};  // opaque black
};

// ── Equality ─────────────────────────────────────────────────────────────────
//
// Field-wise across every member, same discipline and rationale as Style.hpp:
// the panel's "skip unchanged commit" predicate and any future diff/merge
// consumer rely on a total equality. Every editable field participates; add a
// field above and you MUST add it here or edits to it silently fail to commit.
// C++17 doesn't synthesise these (C++20 would), so they are written by hand.

inline bool operator==(const TextStyleHeader& a, const TextStyleHeader& b) {
    return a.id        == b.id
        && a.name      == b.name
        && a.category  == b.category
        && a.parent_id == b.parent_id;
}
inline bool operator!=(const TextStyleHeader& a, const TextStyleHeader& b) { return !(a == b); }

inline bool operator==(const ParaFormat& a, const ParaFormat& b) {
    return a.align           == b.align
        && a.leading_px       == b.leading_px
        && a.indent_left_px   == b.indent_left_px
        && a.indent_right_px  == b.indent_right_px
        && a.indent_first_px  == b.indent_first_px
        && a.tabs             == b.tabs;
}
inline bool operator!=(const ParaFormat& a, const ParaFormat& b) { return !(a == b); }

inline bool operator==(const CharDefaults& a, const CharDefaults& b) {
    return a.family          == b.family
        && a.size            == b.size
        && a.bold            == b.bold
        && a.italic          == b.italic
        && a.letter_spacing  == b.letter_spacing
        && a.colour          == b.colour;
}
inline bool operator!=(const CharDefaults& a, const CharDefaults& b) { return !(a == b); }

inline bool operator==(const TextStyle& a, const TextStyle& b) {
    return a.header == b.header
        && a.para   == b.para
        && a.chars  == b.chars;
}
inline bool operator!=(const TextStyle& a, const TextStyle& b) { return !(a == b); }

} // namespace style
} // namespace Curvz
