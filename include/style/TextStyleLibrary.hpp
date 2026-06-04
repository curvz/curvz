#pragma once
//
// TextStyleLibrary.hpp — two-list registry of named paragraph text styles
// (app built-ins + user), with single-parent inheritance and a resolve pump.
//
// Per text_formatting_design.md §6 — "A paragraph-style system is another
// instance of [the style::StyleLibrary] exact pattern — a new sibling library,
// not a new stylesheet engine." This header is that sibling: the same two-tier
// app/user shell as StyleLibrary (lookup user-first; writes target user only;
// app entries hardcoded at construction and never serialised), the same
// CRUD/signals/JSON-round-trip surface, the same UUID + prefix discipline.
//
// The one thing this library has that StyleLibrary does not: INHERITANCE. Styles
// form a forest of single-parent chains rooted at Default/Body. A style stores
// only its deltas from its parent; resolve() walks the chain to produce a total
// ResolvedTextStyle. This is the only new machinery — everything else is the
// proven pattern.
//
// ── Scope (s340 m1) — pure addition ──────────────────────────────────────────
//   * App + user storage, lookup, CRUD on user, signals, the seeded default set,
//     JSON round-trip of the user tier, and the resolve pump.
//   * NOT wired into CurvzProject yet (no sibling member, no project.json
//     hookup) — the round-trip methods exist; the project calls them in a later
//     milestone.
//   * NO consumer — the fitter/renderer reads ResolvedTextStyle in a later
//     milestone; nothing reads it now.
//   * NO reverse-usage cleanup on remove (no SceneNode style-ref field yet); a
//     removed style's bound runs fall back to the floor when that field lands.
//
// Thread-safety: none. Main-thread only.
//

#include "style/TextStyle.hpp"

#include <sigc++/sigc++.h>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Curvz {
namespace style {

// ── Format-level pumps ────────────────────────────────────────────────────────
//
// Per-TextStyle JSON encode/decode at namespace scope (mirrors the graphic
// library's style_to_json / style_from_json), so a future import/export bridge
// reuses the exact shape used by the project-tier round-trip. One format seam.
//
// Tolerance contract: text_style_from_json returns std::nullopt for entries
// missing header.id, carrying an "app:" id in user data, or with a malformed
// top-level shape. Sparse sub-fields are simply absent when unset (an optional
// field that is unset is NOT written; a missing key reads back as unset).

nlohmann::json           text_style_to_json(const TextStyle& s);
std::optional<TextStyle> text_style_from_json(const nlohmann::json& entry);

class TextStyleLibrary {
public:
    // Constructs with the seeded app-style set (Default/Body root + a macOS-ish
    // handful inheriting from it) pre-populated and the user list empty. See
    // TextStyleLibrary.cpp for the seed contents.
    TextStyleLibrary();

    // --- User-style CRUD ---------------------------------------------------
    // Writes land exclusively on the user list. "app:" ids are read-only; the
    // escape hatch is duplicate_to_user(). Same contract as StyleLibrary.

    // Insert into the user list. Empty header.id -> a fresh "txs_<uuid>". An
    // "app:" id, or an id already present in either list, is rejected (empty
    // return). Fires signal_text_style_added on success.
    TextStyleId add_text_style(TextStyle s);

    // Replace an existing user style in-place. new.header.id must equal `id`.
    // Rejects built-in / not-found / mismatched. Fires signal_text_style_changed
    // on success — the trigger for downstream re-resolve/redraw when consumers
    // land. NOTE the cascade implication: editing a style changes the resolved
    // look of every DESCENDANT that did not override the changed attribute, so
    // a changed-signal listener must re-resolve the subtree, not just the style.
    bool update_text_style(const TextStyleId& id, TextStyle s);

    // Remove a user style. Rejects built-in / not-found. Fires
    // signal_text_style_removed. Children whose parent_id pointed at the removed
    // id are NOT reparented (m1) — their chain dangles and resolve() bottoms at
    // the floor for the inherited fields. Reparent-to-grandparent is a later
    // refinement; the dangling-to-floor degenerate is safe meanwhile.
    bool remove_text_style(const TextStyleId& id);

    // Duplicate a style (from EITHER list) into the user list under a fresh
    // "txs_<uuid>". The copy keeps the source's parent_id (so a duplicated app
    // style still inherits from the same parent) and name + " copy". Returns the
    // new id, or empty on lookup failure.
    TextStyleId duplicate_to_user(const TextStyleId& src);

    // --- Lookup -------------------------------------------------------------

    // Lookup across both lists — user first, then app. nullptr when absent in
    // both. Pointers stay valid until the next mutation of the relevant list.
    const TextStyle* find_text_style(const TextStyleId& id) const;

    // True iff the id refers to a style actually present in the app list
    // (tighter than the free is_built_in_text prefix check).
    bool is_built_in(const TextStyleId& id) const;

    // --- Resolve (the inheritance pump) ------------------------------------
    //
    // Walk the parent chain from `id` up to its root, then overlay the sparse
    // set-fields from root DOWN to `id` onto the hardcoded floor, producing a
    // total ResolvedTextStyle. design §6's resolve_paragraph_format, generalised
    // to carry the character-default half too (a paragraph style owns both).
    //
    //   floor (SceneNode text defaults)  <- root  <- ... <- id
    //   later (at the consumption seam): <- local per-run span overrides
    //
    // The local-override tier (design §7) is NOT applied here — it lives in the
    // buffer spans and layers on at the fitter. This pump is the first two tiers.
    //
    // Robustness: an unknown `id`, an empty library, or a dangling parent each
    // resolve cleanly (an unknown id resolves to the pure floor; a dangling
    // parent stops the walk early). A CYCLE in parent_id (corruption) is broken
    // by a visited-set guard and logged — the walk stops at the repeat.
    ResolvedTextStyle resolve(const TextStyleId& id) const;

    // --- Iteration ----------------------------------------------------------
    // App styles always group above user styles in the panel. Pointers into
    // library-owned storage; valid until the next mutation of the relevant list.

    std::vector<const TextStyle*> app_styles_in_category(const std::string& cat) const;
    std::vector<const TextStyle*> user_styles_in_category(const std::string& cat) const;
    std::vector<std::string> app_categories() const;
    std::vector<std::string> user_categories() const;

    // --- Counts -------------------------------------------------------------

    std::size_t app_style_count()  const { return m_app_styles.size(); }
    std::size_t user_style_count() const { return m_user_styles.size(); }
    bool empty() const {
        return m_app_styles.empty() && m_user_styles.empty();
    }

    // --- JSON round-trip (user tier only) ----------------------------------
    // Mirrors StyleLibrary::to_user_json / from_user_json: app styles are
    // hardcoded, never serialised, re-seeded every launch. from_user_json CLEARS
    // the user tier first (atomic replace, fires no signals); incoming entries
    // with empty / "app:" ids are skipped. Writes a top-level "text_styles"
    // array (distinct key from the graphic library's "styles").

    void to_user_json(nlohmann::json& j) const;
    void from_user_json(const nlohmann::json& j);

    // --- Signals ------------------------------------------------------------
    // Three fine-grained id-carrying signals, same pattern as StyleLibrary. See
    // update_text_style for the cascade implication on the changed signal.

    using TextStyleIdSignal = sigc::signal<void(TextStyleId)>;

    TextStyleIdSignal& signal_text_style_added()   { return m_sig_added; }
    TextStyleIdSignal& signal_text_style_changed() { return m_sig_changed; }
    TextStyleIdSignal& signal_text_style_removed() { return m_sig_removed; }

private:
    // Fresh "txs_<uuid>" id that collides with nothing in either list. Same UUID
    // source as SceneNode (generate_internal_id); one defensive retry.
    TextStyleId generate_unique_user_id() const;

    // Linear-scan lookups. Lists are small (sub-100); switch to a map keyed on
    // id if profiling ever shows this hot — the API doesn't change.
    std::vector<TextStyle>::iterator       find_user(const TextStyleId& id);
    std::vector<TextStyle>::const_iterator find_user(const TextStyleId& id) const;
    std::vector<TextStyle>::const_iterator find_app(const TextStyleId& id) const;

    // Overlay a single sparse style's set-fields onto an accumulating resolved
    // value (used by resolve() while walking root->leaf). Each optional that is
    // set wins; unset leaves the accumulator untouched. One place to maintain
    // when a field is added to ParaFormat / CharDefaults.
    static void overlay(ResolvedTextStyle& acc, const TextStyle& s);

    // Two lists. Vector iteration is insertion-order == panel display order. The
    // app list is populated once at construction and never mutated; its const-
    // ness at the behaviour level is enforced by every CRUD path checking
    // is_built_in_text() first.
    std::vector<TextStyle> m_app_styles;
    std::vector<TextStyle> m_user_styles;

    TextStyleIdSignal m_sig_added;
    TextStyleIdSignal m_sig_changed;
    TextStyleIdSignal m_sig_removed;
};

} // namespace style
} // namespace Curvz
