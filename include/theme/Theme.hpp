#pragma once
//
// Theme.hpp — a named, reusable bundle of doc-level "non-physical" settings
// (S103 m1).
//
// Per the S102 → S103 handoff "Theme System":
//   A Theme is the document-settings analogue of Style. Where Style bundles
//   appearance (fill + stroke + shadow) and binds to objects, Theme bundles
//   doc-level customisation (units, background, guides, grid, margins, snap)
//   and applies to documents. The shape — header + sub-structs + a top-level
//   record — mirrors Style System exactly.
//
// Why themes (vs a "Sync Document Settings" menu item):
//   The original ask was a one-shot sync: pick source doc, pick targets,
//   copy across. Generalising into named, reusable bundles costs almost
//   nothing extra (Theme System rides on the same architectural rails as
//   Style System) and gives users a "Print Setup" / "Web Sketch" preset
//   library they can save and reuse across projects via JSON pack
//   import/export (m2, mirrors S102's style packs).
//
// What's NOT a theme (the "physical / structural" cut):
//   Canvas width / height / DPI / orientation / display mode — those
//   describe the document's geometry and shouldn't be silently rewritten
//   by an apply.
//   Document name / content / layer structure — content, not settings.
//   Project-scoped libraries (swatches, styles, themes themselves) — not
//   per-doc.
//   measure_* fields — ephemeral measure-tool state, not "settings".
//
// Identity scheme (mirrors Style):
//   * User themes: id prefix "thm_" + UUID (g_uuid_string_random via
//     generate_internal_id()). Serialised with the project. Editable.
//   * Built-in (app:) themes: NONE in v1. The is_built_in() predicate and
//     two-list architecture are kept for symmetry — adding curated app
//     themes later is a one-line change to the (currently empty) seed.
//   The predicate is the gate every CRUD path checks before accepting a
//   write. Same shape as StyleLibrary, even though no app themes exist
//   yet — the contract is in place so future curated themes don't
//   require a refactor.
//
// Apply contract (set in stone for v1):
//   * Apply is atomic — the whole bundle writes, no per-group cherry-pick.
//     Per-group checkboxes (e.g. "apply units but not colours") are a
//     deferred enhancement.
//   * Apply is one-shot — no bindings. Once a theme is applied to a doc,
//     the doc has those values. No `bound_theme` field, no live ripple
//     when the theme is later edited. "Apply and forget."
//   * Apply is NOT undoable in v1 — the dialog will warn the user. Themes
//     are infrequent operations on doc settings; making them undoable
//     would mean snapshotting every targeted doc's pre-apply state into
//     a CompositeCommand, which is heavy for the use case. Import (the
//     CRUD-on-library side) IS undoable, mirroring StyleIO; that's
//     a library mutation, not a doc mutation.
//

#include "UnitSystem.hpp"      // Unit
#include "CurvzDocument.hpp"   // SnapSettings (existing struct, reused)

#include <string>

namespace Curvz {
namespace theme {

// Project-scoped handle. Mirrors StyleId — string for ergonomics, prefix
// is the only structure imposed.
using ThemeId = std::string;

// True iff the id refers to an app (built-in) theme. v1 has no app themes
// so this is always false in practice, but the predicate is in place for
// the same is-this-read-only? gate every CRUD path consults. See Style.hpp
// for the broader rationale.
inline bool is_built_in(const ThemeId& id) {
    static constexpr char prefix[] = "app:";
    static constexpr std::size_t prefix_len = sizeof(prefix) - 1;
    return id.size() >= prefix_len &&
           id.compare(0, prefix_len, prefix) == 0;
}

// Metadata shared by every theme. Lives at the head of each Theme. The
// flat single-string `category` is intentional — same nesting decision
// as Style, no folder hierarchy in v1.
struct ThemeHeader {
    // Project-scoped unique id. "thm_<uuid>" for user themes; "app:<slug>"
    // reserved for future built-ins. is_built_in() is the discriminant.
    ThemeId id;

    // Display name, user-editable. UI never falls back to id when empty —
    // empty is sanitised at the library-mutation layer (auto-generated
    // disambiguation). Per project-wide policy: UUIDs never appear in
    // user-facing UI.
    std::string name;

    // Section header in the dialog's library list. Empty string means
    // "uncategorised". Equality is exact (case-sensitive); no
    // normalisation. v1 may not use categories visibly — the dialog
    // groups by exact string only if any theme has a non-empty category.
    std::string category;
};

// ── Sub-bundles ─────────────────────────────────────────────────────────────
//
// One sub-struct per Inspector section that the theme captures, with the
// SAME field names the doc / GridLayer / MarginLayer carry. The apply
// funnel does a 1:1 copy — naming convergence keeps the funnel boring
// and the JSON shape obvious.
//
// Defaults mirror the doc / SceneNode struct defaults so a freshly-
// default-constructed Theme is a sensible "factory baseline" — applying
// it would reset a doc to fresh-out-of-the-box appearance.

// Display-unit preference. Affects rulers and inspector readouts; does
// NOT affect canvas geometry. (Geometry is the canvas-mode layer, which
// is OUT of theme scope per the design.)
struct UnitSettings {
    Unit display_unit = Unit::Px;
};

// Editor presentation backgrounds (REMOVED in s116 m6).
//
// Originally a per-doc theme field. Removed when the workspace appearance
// (artboard / workspace bg / motif) moved to CurvzProject. Themes describe
// reusable doc-level settings (units, guides, grid, margins, snap) — bg
// is project-scope and lives on CurvzProject, not in themes.
//
// Old theme JSON files (if any exist) carrying a "background" key load
// fine: ThemeLibrary's bg_from_json was removed and the read site now
// just ignores the key.

// Guide line colour and visibility.
//
// Visibility lives on the GuideLayer SceneNode (the SceneNode-level
// `visible` field). The colour lives on the document directly (not the
// layer, historically — see CurvzDocument::guide_color_*). Apply walks
// both.
struct GuideSettings {
    double color_r = 0.0;
    double color_g = 0.749;       // matches CurvzDocument default (cyan)
    double color_b = 1.0;

    // SceneNode::visible on the GuideLayer. True by default — guides
    // are usually shown. Apply respects layer presence (a doc without
    // a GuideLayer keeps things that way; we don't synthesise one).
    bool visible = true;
};

// Grid layer settings. Field names match SceneNode's `grid_*` fields
// exactly (those fields are only meaningful on a GridLayer; see
// SceneNode.hpp comments).
//
// `enabled` is the "is the GridLayer present in the doc at all?" gate —
// matches the Inspector's "Enable" checkbox. False means apply removes
// any existing GridLayer; true means apply ensures one exists, then
// writes the rest.
struct GridSettings {
    bool   enabled    = false;    // GridLayer present in the doc?
    bool   visible    = true;     // SceneNode::visible on the GridLayer
    double spacing_x  = 100.0;    // SceneNode::grid_spacing_x default
    double spacing_y  = 100.0;
    double offset_x   = 0.0;
    double offset_y   = 0.0;
    double color_r    = 0.5;
    double color_g    = 0.5;
    double color_b    = 0.8;
    double color_a    = 0.35;
    bool   dots       = false;    // false=lines, true=dots at intersections
};

// Margin layer settings. Field names match SceneNode's `margin_*` fields.
// Same `enabled` gate semantics as GridSettings.
struct MarginSettings {
    bool   enabled   = false;     // MarginLayer present in the doc?
    bool   visible   = true;      // SceneNode::visible on the MarginLayer
    double top       = 0.0;
    double bottom    = 0.0;
    double left      = 0.0;
    double right     = 0.0;
    int    columns   = 1;
    double col_gap   = 0.0;
    int    rows      = 1;
    double row_gap   = 0.0;
    double color_r   = 0.8;
    double color_g   = 0.3;
    double color_b   = 0.3;
    double color_a   = 0.15;
};

// Snap settings — alias to the existing per-doc SnapSettings struct in
// CurvzDocument.hpp. We don't redefine — that would invite the two
// definitions to drift apart. The field set stays in lockstep with the
// canonical one because we ARE the canonical one, just renamed.
//
// Keeping the alias means adding a new snap field is a one-line edit on
// CurvzDocument.hpp and the theme picks it up automatically — at the
// cost of having to remember it round-trips through JSON. The
// per-Theme JSON pump enumerates each field explicitly, so a new one
// has to be wired there too; that's fine, the format seam is already
// the place format-aware code lives.
using ThemeSnapSettings = SnapSettings;

// ── The Theme record ────────────────────────────────────────────────────────
//
// Header + the six sub-bundles, in inspector order (matches the
// Properties Panel section ordering — Canvas/Units, Background, Guides,
// Grid, Margins, Snap). Apply walks them in this order; any sub-bundle
// with no doc-side dependency on a layer (units, background, guides
// colour, snap) is straightforward, and the layer-bound ones (grid,
// margins) handle the create-or-destroy-as-needed logic in the apply
// funnel.

struct Theme {
    ThemeHeader        header;
    UnitSettings       units;
    // BackgroundSettings background; — removed in s116 m6 (moved to CurvzProject)
    GuideSettings      guides;
    GridSettings       grid;
    MarginSettings     margins;
    ThemeSnapSettings  snap;
};

// ── Equality ────────────────────────────────────────────────────────────────
//
// S98 lesson, applied here: every editable field participates in
// equality, no carve-outs. The dialog's "skip unchanged commit"
// predicate (m3) will use these — a missing field comparison would
// silently drop edits to that field through the same failure mode the
// Style System hit at m4d.
//
// Why defaulted operator== isn't used: C++17 doesn't synthesise it
// (C++20 feature). We write each one by hand, comparing every member
// with no elision. If you add a field to any of these structs and
// don't add it here, the dialog's skip-unchanged predicate will match
// and silently drop edits to your new field. One place to update;
// breakage is visible if you forget.

inline bool operator==(const ThemeHeader& a, const ThemeHeader& b) {
    return a.id       == b.id
        && a.name     == b.name
        && a.category == b.category;
}
inline bool operator!=(const ThemeHeader& a, const ThemeHeader& b) { return !(a == b); }

inline bool operator==(const UnitSettings& a, const UnitSettings& b) {
    return a.display_unit == b.display_unit;
}
inline bool operator!=(const UnitSettings& a, const UnitSettings& b) { return !(a == b); }

inline bool operator==(const GuideSettings& a, const GuideSettings& b) {
    return a.color_r == b.color_r
        && a.color_g == b.color_g
        && a.color_b == b.color_b
        && a.visible == b.visible;
}
inline bool operator!=(const GuideSettings& a, const GuideSettings& b) { return !(a == b); }

inline bool operator==(const GridSettings& a, const GridSettings& b) {
    return a.enabled   == b.enabled
        && a.visible   == b.visible
        && a.spacing_x == b.spacing_x
        && a.spacing_y == b.spacing_y
        && a.offset_x  == b.offset_x
        && a.offset_y  == b.offset_y
        && a.color_r   == b.color_r
        && a.color_g   == b.color_g
        && a.color_b   == b.color_b
        && a.color_a   == b.color_a
        && a.dots      == b.dots;
}
inline bool operator!=(const GridSettings& a, const GridSettings& b) { return !(a == b); }

inline bool operator==(const MarginSettings& a, const MarginSettings& b) {
    return a.enabled == b.enabled
        && a.visible == b.visible
        && a.top     == b.top
        && a.bottom  == b.bottom
        && a.left    == b.left
        && a.right   == b.right
        && a.columns == b.columns
        && a.col_gap == b.col_gap
        && a.rows    == b.rows
        && a.row_gap == b.row_gap
        && a.color_r == b.color_r
        && a.color_g == b.color_g
        && a.color_b == b.color_b
        && a.color_a == b.color_a;
}
inline bool operator!=(const MarginSettings& a, const MarginSettings& b) { return !(a == b); }

// SnapSettings (the CurvzDocument struct we aliased) doesn't yet have an
// operator==. Define it here because that's where the equality contract
// for the Theme-as-a-whole is enforced — keeping it next to the other
// theme-side equalities makes "every editable field participates" easy
// to audit. If SnapSettings later grows an operator== of its own, this
// definition becomes redundant and should be removed; the in-class one
// would shadow it.
inline bool operator==(const ThemeSnapSettings& a, const ThemeSnapSettings& b) {
    return a.enabled      == b.enabled
        && a.snap_guides  == b.snap_guides
        && a.snap_grid    == b.snap_grid
        && a.snap_margins == b.snap_margins
        && a.snap_nodes   == b.snap_nodes
        && a.snap_edges   == b.snap_edges
        && a.snap_centers == b.snap_centers;
}
inline bool operator!=(const ThemeSnapSettings& a, const ThemeSnapSettings& b) { return !(a == b); }

inline bool operator==(const Theme& a, const Theme& b) {
    return a.header     == b.header
        && a.units      == b.units
        // a.background — removed in s116 m6 (bg moved to CurvzProject).
        && a.guides     == b.guides
        && a.grid       == b.grid
        && a.margins    == b.margins
        && a.snap       == b.snap;
}
inline bool operator!=(const Theme& a, const Theme& b) { return !(a == b); }

// ── Capture / Apply free functions ──────────────────────────────────────────
//
// These are the bridge between a Theme value and a CurvzDocument. They
// don't live on the library because the library doesn't own documents;
// it owns themes.
//
// Apply contract (recap from top of file): atomic, one-shot, no
// bindings. Each sub-bundle's values write into the doc's corresponding
// fields. Layer-presence settings (grid.enabled, margins.enabled) drive
// ensure_X_layer() / removal of the layer respectively.
//
// `theme.header` is metadata only; apply does NOT touch doc->filename
// or anything else identity-shaped. Renaming a theme doesn't rename
// the doc it was applied to — that's a doc-level concern.

// Snapshot a doc into a fresh Theme (header empty — caller fills).
// Used by the "From document" source mode in m3's dialog: pick a doc,
// the dialog reads its current values into a transient Theme, then
// applies that to the targets.
//
// Pure read of doc — no mutation. Returns a value-typed Theme that the
// caller can either apply directly or mutate further (e.g. set a name
// before passing to add_theme).
Theme capture_theme_from_doc(const CurvzDocument& doc);

// Write theme's settings into doc. Atomic — no diffing, no skip-unchanged
// at this layer (the dialog handles "did anything change at all" before
// calling). Layer-presence changes run first (so subsequent field writes
// see the right layers); then doc-level fields; then per-layer fields.
//
// NOT undoable. The dialog warns the user before invoking apply on a
// target list. See top-of-file rationale.
void apply_theme_to_doc(const Theme& theme, CurvzDocument& doc);

} // namespace theme
} // namespace Curvz
