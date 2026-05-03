#pragma once
//
// StyleLibrary.hpp — two-list registry of styles (app built-ins + user).
//
// Per ARCHITECTURE.md "Style System":
//   StyleLibrary is in-memory state held on CurvzProject (sibling to
//   SwatchLibrary). Internally TWO lists — app (hardcoded at construction,
//   read-only at the library's behaviour level) and user (per-project,
//   fully mutable). Reads check user first, then app. Writes target user
//   only.
//
//   App styles are identified by their "app:" id prefix (see Style.hpp
//   is_built_in); user styles by "stl_". The library doesn't validate
//   prefixes itself beyond the is_built_in() gate on writes; the
//   discriminant is a contract the data carries.
//
// What's different from SwatchLibrary:
//   * No defaults file. App styles are a hardcoded init list in the
//     constructor — no JSON, no Application::on_activate seeding step.
//     This is a Phase 1 simplification; the curated app-style set lands
//     in Phase 2 and may move to JSON if it gets large.
//   * No recents, no active-something. Styles aren't selected by tool
//     state the way swatches are.
//   * No set_paint chokepoint here. The mutate-appearance funnel lives
//     in StyleInterop — it's the chokepoint for break-on-override
//     correctness, and it fires *outside* the library because the
//     library doesn't own SceneNodes.
//
// Phase 1 m1 scope (this milestone):
//   * App + user storage, lookup, CRUD on user, signals.
//   * 3 hardcoded app-style stubs at construction (decision and content
//     in StyleLibrary.cpp).
//   * NO JSON round-trip. User styles are not persisted yet — they live
//     for the lifetime of the loaded project. Round-trip lands in m3.
//   * NO usage-tracking or reverse-index walk on delete. The cross-doc
//     propagation walk (signal_style_changed → re-materialise bound
//     SceneNodes) also lands in m3 once SceneNode has bound_style.
//
// Thread-safety: none. Main-thread only, mutated on the GTK main loop.
//

#include "style/Style.hpp"

#include <sigc++/sigc++.h>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Curvz {
namespace style {

// ── Format-level pumps (S102 m1) ──────────────────────────────────────
//
// Per-Style JSON encode/decode. These were previously file-scope helpers
// inside StyleLibrary.cpp; lifted to namespace scope so the StyleIO
// import/export bridge (S102 m1) can reuse the exact same shape used
// by the project-tier round-trip (to_user_json / from_user_json). The
// format seam stays in one place — both consumers go through the same
// pump, so a future field addition only has to update one writer/reader
// pair.
//
// Tolerance contract: style_from_json returns std::nullopt for entries
// missing header.id, carrying an "app:" id in user data, or with a
// malformed top-level shape. Individual sub-blocks (fill/stroke/shadow)
// are permissively defaulted when missing.

nlohmann::json     style_to_json(const Style& s);
std::optional<Style> style_from_json(const nlohmann::json& entry);

class StyleLibrary {
public:
    // Constructs with the hardcoded app-style stub list pre-populated and
    // the user list empty. See StyleLibrary.cpp for the stub contents.
    StyleLibrary();

    // --- User-style CRUD ---------------------------------------------------
    //
    // Writes land exclusively on the user list. An attempt to add a style
    // whose header.id is "app:..." is rejected (returns empty). update /
    // remove on an "app:" id are also rejected — the UI's escape hatch is
    // duplicate_to_user(), which produces a "stl_..." copy.

    // Insert a style into the user list. If the input's header.id is empty,
    // a fresh "stl_<uuid>" id is generated. If the id is already in either
    // list, or is an "app:" id, the insert is rejected and an empty StyleId
    // is returned.
    //
    // Fires signal_style_added on success.
    StyleId add_style(Style s);

    // Replace an existing user style in-place. The new style's header.id
    // must equal `id`. Returns false if the id is built-in (read-only),
    // not found in the user list, or mismatched against the input.
    //
    // Fires signal_style_changed on success. The signal is the trigger
    // for cross-document propagation (re-materialise bound SceneNodes,
    // queue redraw); that walk wires up in m3 once SceneNode has
    // bound_style.
    bool update_style(const StyleId& id, Style s);

    // Remove a user style. Returns false if the id is built-in or not
    // found in the user list.
    //
    // Fires signal_style_removed on success. Reverse-usage cleanup
    // (clearing bound_style on every SceneNode that referenced the
    // removed id) is the listener's responsibility — m3 wires that up.
    bool remove_style(const StyleId& id);

    // Duplicate a style (from EITHER list) into the user list under a
    // fresh "stl_<uuid>" id. Always succeeds for an existing id; this
    // is how the UI lets users "edit" an app style.
    //
    // The copy inherits the source style's name (with " copy" appended
    // for disambiguation in the panel). Category is preserved verbatim.
    //
    // Returns the new id, or empty StyleId on lookup failure.
    StyleId duplicate_to_user(const StyleId& src);

    // --- Lookup -------------------------------------------------------------

    // Lookup across both lists — user first, then app. Returns nullptr
    // when absent in both. The user-first ordering exists for symmetry
    // with SwatchLibrary; in practice user and app ids share no prefix
    // so a collision is impossible by construction.
    const Style* find_style(const StyleId& id) const;

    // True iff the id refers to a style in the app list. Convenience
    // forwarder around the free is_built_in() predicate, but also
    // sanity-checks that the id actually exists in the app list (so
    // a typo'd "app:nonexistent" returns false, not true).
    bool is_built_in(const StyleId& id) const;

    // --- Iteration ----------------------------------------------------------
    //
    // The panel UI iterates by category, with app styles always grouped
    // above user styles (addendum decision 10). These accessors return
    // pointers into library-owned storage; pointers stay valid until
    // the next mutation of the relevant list.

    // App styles in the given category, in insertion order. Empty
    // category string returns app styles whose category is "".
    std::vector<const Style*> app_styles_in_category(const std::string& cat) const;

    // User styles in the given category, in insertion order.
    std::vector<const Style*> user_styles_in_category(const std::string& cat) const;

    // Distinct user-style categories, in insertion order (first-seen).
    // Used by the panel to render section headers.
    std::vector<std::string> user_categories() const;

    // Distinct app-style categories, in insertion order. Symmetric to
    // user_categories(); the panel uses both to assemble the full list
    // of section headers.
    std::vector<std::string> app_categories() const;

    // --- Counts -------------------------------------------------------------

    std::size_t app_style_count()  const { return m_app_styles.size(); }
    std::size_t user_style_count() const { return m_user_styles.size(); }

    // True iff both lists are empty. App list is populated at
    // construction so this is normally false; included for symmetry
    // with SwatchLibrary::empty() and for tests that want to assert
    // a freshly-defaulted state (i.e. with the app stub list also
    // wiped — uncommon).
    bool empty() const {
        return m_app_styles.empty() && m_user_styles.empty();
    }

    // --- JSON round-trip ---------------------------------------------------
    //
    // S76 m3b. Serialises the USER tier only — app styles are hardcoded
    // in the constructor, never serialised, re-seeded every launch.
    // Mirrors SwatchLibrary::to_custom_json / from_custom_json in shape
    // and tolerance: .value(...) defaulting, permissive on partial data,
    // LOG_WARN on dropped entries.
    //
    // from_user_json CLEARS the user tier before reading. Incoming
    // entries with empty ids or "app:" prefix are skipped (defensive —
    // app ids belong only to the hardcoded list). Fires no signals; the
    // load path is an atomic replace, not a stream of CRUD operations,
    // and listeners observing a mid-load intermediate state would see
    // a briefly-inconsistent library. CurvzProject::load is expected
    // to follow up with a redraw/queue_draw at project level.
    //
    // to_user_json writes a top-level "styles" array. The per-entry
    // shape is documented in StyleLibrary.cpp.

    void to_user_json(nlohmann::json& j) const;
    void from_user_json(const nlohmann::json& j);

    // --- Signals ------------------------------------------------------------
    //
    // Three fine-grained signals so listeners (Phase 2 panel UI, Phase 3
    // import/export, the cross-doc propagation walk in m3) can react to
    // exactly the change kind they care about. Mirrors SwatchLibrary's
    // signal_swatch_changed pattern; carries the id so listeners can
    // filter the scene walk to just the affected bindings.
    //
    // Renames don't fire signal_style_changed — the id (the binding key)
    // is immutable; only the display name shifts, and bound objects
    // don't care about names. The panel listens on the same signal as
    // it does for full edits and re-renders.
    //
    // Actually — Phase 1 m1 doesn't expose a separate rename path
    // (rename goes through update_style with a new header.name on the
    // same id). So signal_style_changed fires for renames too. The
    // cross-doc propagation walk in m3 should be a no-op when only the
    // name differs (cheap shallow check before re-materialising).

    using StyleIdSignal = sigc::signal<void(StyleId)>;

    StyleIdSignal& signal_style_added()   { return m_sig_added; }
    StyleIdSignal& signal_style_changed() { return m_sig_changed; }
    StyleIdSignal& signal_style_removed() { return m_sig_removed; }

private:
    // Generate a fresh "stl_<uuid>" id that doesn't collide with anything
    // in either list. Same UUID source as SceneNode (g_uuid_string_random
    // via generate_internal_id()). With 122 bits of entropy a collision
    // is not a practical concern; one defensive retry covers RNG pathology.
    StyleId generate_unique_user_id() const;

    // Find a style in the user list by id. Linear scan — Phase 1 lists
    // are small (sub-100 entries expected). If profiling later shows this
    // hot, switch to std::map keyed on id; the API doesn't change.
    std::vector<Style>::iterator       find_user(const StyleId& id);
    std::vector<Style>::const_iterator find_user(const StyleId& id) const;

    // Same for the app list. Public lookup goes through find_style which
    // checks user first; this is the second-half lookup.
    std::vector<Style>::const_iterator find_app(const StyleId& id) const;

    // Two lists. Vector iteration is insertion-order, which is also panel
    // display order. App list is populated once at construction and never
    // mutated — its const-ness at the behaviour level is enforced by
    // every CRUD path checking is_built_in() before touching anything.
    std::vector<Style> m_app_styles;
    std::vector<Style> m_user_styles;

    StyleIdSignal m_sig_added;
    StyleIdSignal m_sig_changed;
    StyleIdSignal m_sig_removed;
};

} // namespace style
} // namespace Curvz
