#pragma once
//
// ThemeLibrary.hpp — two-list registry of themes (S103 m1).
//
// Per the S102 → S103 handoff "Theme System":
//   ThemeLibrary is in-memory state held on CurvzProject (sibling to
//   SwatchLibrary and StyleLibrary). Same two-list architecture even
//   though v1 has no built-in app themes — the contract is in place
//   for symmetry with the rest of the project's library systems and
//   so adding curated themes in a later release is a one-line edit.
//
//   Reads check user first, then app. Writes target user only. App
//   themes (when they exist) are identified by their "app:" id prefix
//   (see Theme.hpp is_built_in); user themes by "thm_". The library
//   doesn't validate prefixes itself beyond the is_built_in() gate
//   on writes; the discriminant is a contract the data carries.
//
// What's the same as StyleLibrary:
//   * Two-list shape, app + user.
//   * CRUD on user only; built-in writes rejected.
//   * UUID-prefixed user ids (thm_<uuid>).
//   * Signal trio (added / changed / removed) for library state.
//   * JSON round-trip pumps (to_user_json / from_user_json) at
//     namespace scope so the m2 import/export bridge can reuse them.
//
// What's different:
//   * No app stub list at construction. App themes deferred indefinitely.
//   * No bound_theme on documents — themes don't bind. Apply is one-shot.
//     There's no cross-doc propagation walk to wire here, no
//     UpdateThemeCommand needed (the dialog re-applies to a doc set
//     instead of editing-in-place with a propagation), and signal_changed
//     is consumed only by the dialog itself for refreshing its library
//     list.
//   * A FOURTH signal beyond the StyleLibrary trio (s227 m1):
//     signal_theme_applied. Fired by apply drivers (ThemesPanel,
//     ThemesScriptable), NOT by the library itself. Exists because
//     apply doesn't mutate the library — the other three signals never
//     fire for apply — but scripted callers need a hook to drive the
//     canvas refresh cascade that the panel does directly. See the
//     Signals block below for the driver / listener contract.
//
// Thread-safety: none. Main-thread only, mutated on the GTK main loop.
//

#include "theme/Theme.hpp"

#include <sigc++/sigc++.h>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Curvz {
namespace theme {

// ── Format-level pumps ──────────────────────────────────────────────────
//
// Per-Theme JSON encode/decode at namespace scope. Mirrors S102 m1's lift
// of style_to_json / style_from_json — both the project-tier round-trip
// (to_user_json / from_user_json) and the m2 ThemeIO bridge call these,
// so a future field addition only updates one writer/reader pair.
//
// Tolerance contract: theme_from_json returns std::nullopt for entries
// missing header.id, carrying an "app:" id in user data, or with a
// malformed top-level shape. Individual sub-blocks (units / background
// / guides / grid / margins / snap) are permissively defaulted when
// missing — older project.json files that pre-date theme additions
// load with the new fields at struct-default values.

nlohmann::json     theme_to_json(const Theme& t);
std::optional<Theme> theme_from_json(const nlohmann::json& entry);

class ThemeLibrary {
public:
    // Constructs with empty user and empty app lists. v1: no app themes
    // are seeded. The two-list shape is preserved for symmetry with
    // StyleLibrary; future curated app themes would seed in here.
    ThemeLibrary();

    // --- User-theme CRUD --------------------------------------------------
    //
    // Same shape as StyleLibrary's CRUD, including the "app:"-id rejection
    // contract on writes. The escape hatch for "edit a built-in" is
    // duplicate_to_user(); v1 has no built-ins so this is academic, but
    // the API is in place.

    // Insert a theme into the user list. If the input's header.id is
    // empty, a fresh "thm_<uuid>" id is generated. If the id is already
    // in either list, or is an "app:" id, the insert is rejected and an
    // empty ThemeId is returned.
    //
    // Fires signal_theme_added on success.
    ThemeId add_theme(Theme t);

    // Replace an existing user theme in-place. The new theme's header.id
    // must equal `id`. Returns false if the id is built-in (read-only,
    // when app themes exist), not found in the user list, or mismatched
    // against the input.
    //
    // Fires signal_theme_changed on success. Listeners (the m3 dialog)
    // refresh their library list view; nothing else cares — themes
    // don't bind to docs, so a theme edit doesn't ripple anywhere.
    bool update_theme(const ThemeId& id, Theme t);

    // Remove a user theme. Returns false if the id is built-in or not
    // found in the user list.
    //
    // Fires signal_theme_removed on success. No reverse-usage cleanup
    // needed (no bindings).
    bool remove_theme(const ThemeId& id);

    // Duplicate a theme (from EITHER list) into the user list under a
    // fresh "thm_<uuid>" id. Always succeeds for an existing id; in v1
    // with no app themes, this is just "duplicate a user theme" (the
    // dialog uses it for the "Duplicate" management action).
    //
    // The copy inherits the source theme's name with " copy" appended
    // for disambiguation. Category is preserved verbatim.
    //
    // Returns the new id, or empty ThemeId on lookup failure.
    ThemeId duplicate_to_user(const ThemeId& src);

    // --- Lookup -----------------------------------------------------------

    // Lookup across both lists — user first, then app. Returns nullptr
    // when absent in both.
    const Theme* find_theme(const ThemeId& id) const;

    // True iff the id refers to a theme in the app list. Convenience
    // forwarder around the free is_built_in() predicate, but also
    // sanity-checks that the id actually exists in the app list. v1
    // returns false for every input (app list empty); kept for
    // forward-compat when app themes land.
    bool is_built_in(const ThemeId& id) const;

    // --- Iteration --------------------------------------------------------

    // App themes in the given category, in insertion order. Empty
    // category string returns app themes whose category is "". v1
    // always returns empty.
    std::vector<const Theme*> app_themes_in_category(const std::string& cat) const;

    // User themes in the given category, in insertion order.
    std::vector<const Theme*> user_themes_in_category(const std::string& cat) const;

    // Distinct user-theme categories, in insertion order (first-seen).
    std::vector<std::string> user_categories() const;

    // Distinct app-theme categories. v1 always returns empty.
    std::vector<std::string> app_categories() const;

    // --- Counts -----------------------------------------------------------

    std::size_t app_theme_count()  const { return m_app_themes.size(); }
    std::size_t user_theme_count() const { return m_user_themes.size(); }

    bool empty() const {
        return m_app_themes.empty() && m_user_themes.empty();
    }

    // --- Name uniqueness --------------------------------------------------
    //
    // True iff `name` is already in use by some theme in EITHER list.
    // The library doesn't enforce uniqueness on add_theme (mirroring
    // StyleLibrary — names are display metadata, not the binding key);
    // callers that want disambiguation walk this query and append
    // " 2" / " 3" suffixes themselves. The ThemeIO bridge uses this
    // pattern; the dialog will too for "Save current as theme".
    bool has_user_name(const std::string& name) const;

    // --- JSON round-trip --------------------------------------------------
    //
    // User-tier only. App themes (when they exist) are hardcoded in the
    // constructor and never serialised — re-seeded every launch. Mirrors
    // StyleLibrary::to_user_json / from_user_json shape and tolerance:
    // .value(...) defaulting, permissive on partial data, LOG_WARN on
    // dropped entries.
    //
    // from_user_json CLEARS the user tier before reading. Incoming
    // entries with empty ids or "app:" prefix are skipped (defensive).
    // Fires no signals; the load path is an atomic replace.
    //
    // to_user_json writes a top-level "themes" array.

    void to_user_json(nlohmann::json& j) const;
    void from_user_json(const nlohmann::json& j);

    // --- Signals ----------------------------------------------------------
    //
    // Four fine-grained signals. The first three mirror StyleLibrary's
    // pattern (added / changed / removed); the fourth (s227 m1) is
    // theme-specific: there's no "applied a style to a SceneNode" event
    // in the style world (style binds via bound_style, so apply IS
    // signal_style_changed for the SceneNode side), but for themes apply
    // IS a distinct concern because themes don't bind — apply_theme_to_doc
    // writes doc fields directly with no library mutation, so no other
    // signal fires.
    //
    // Renames don't fire signal_theme_changed independently of update
    // — rename goes through update_theme with a new header.name on the
    // same id, so signal_theme_changed fires for any update including
    // rename-only.
    //
    // signal_theme_applied (s227 m1) is fired by APPLY DRIVERS, NOT by
    // the library itself or by apply_theme_to_doc. The library doesn't
    // see apply at all (apply touches the doc, not the library); the
    // funnel is pure and has no project pointer for snap-mirror or
    // canvas-refresh anyway. The two known drivers today:
    //
    //   * ThemesPanel::on_apply_clicked — panel calls the funnel then
    //     m_on_changed (canvas refresh + inspector + schedule_save), then
    //     fires this signal so any out-of-band listener (the macro
    //     recorder, scripting log, etc.) sees the event. v1 has no such
    //     listener wired in the panel path; the signal exists for the
    //     symmetry below.
    //
    //   * ThemesScriptable's apply verb (s227 m1) — the Scriptable has
    //     no MainWindow pointer and so can't run the canvas-refresh
    //     cascade directly. It fires signal_theme_applied; MainWindow's
    //     zone wiring connects this signal to the same callback body
    //     `m_themes.set_on_changed` installs. That makes scripted apply
    //     refresh the canvas / inspector / save mark, matching panel
    //     apply's visible effect.
    //
    // Same ThemeIdSignal shape as the other three; carries the applied
    // theme's id. Listeners that don't care about the id (refresh-only
    // listeners) take the arg and ignore it.
    using ThemeIdSignal = sigc::signal<void(ThemeId)>;

    ThemeIdSignal& signal_theme_added()   { return m_sig_added; }
    ThemeIdSignal& signal_theme_changed() { return m_sig_changed; }
    ThemeIdSignal& signal_theme_removed() { return m_sig_removed; }
    ThemeIdSignal& signal_theme_applied() { return m_sig_applied; }   // s227 m1

private:
    // Generate a fresh "thm_<uuid>" id that doesn't collide with anything
    // in either list. Same UUID source as StyleLibrary / SceneNode
    // (g_uuid_string_random via generate_internal_id()). With 122 bits
    // of entropy, two consecutive collisions indicate RNG pathology;
    // logged and an empty ThemeId returned.
    ThemeId generate_unique_user_id() const;

    std::vector<Theme>::iterator       find_user(const ThemeId& id);
    std::vector<Theme>::const_iterator find_user(const ThemeId& id) const;
    std::vector<Theme>::const_iterator find_app(const ThemeId& id) const;

    std::vector<Theme> m_app_themes;     // empty in v1
    std::vector<Theme> m_user_themes;

    ThemeIdSignal m_sig_added;
    ThemeIdSignal m_sig_changed;
    ThemeIdSignal m_sig_removed;
    ThemeIdSignal m_sig_applied;   // s227 m1 — fired by apply drivers,
                                   // not by the library itself.
};

} // namespace theme
} // namespace Curvz
