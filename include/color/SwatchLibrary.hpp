#pragma once
//
// SwatchLibrary.hpp — two-tier registry of swatches and palettes.
//
// Per ARCHITECTURE.md "Color System" (Phase 5 refactor):
//   SwatchLibrary is in-memory state held on CurvzProject. Internally it
//   carries TWO pools — defaults (shipped by the app, read-only at the
//   library's behaviour level) and custom (per-project, fully mutable).
//   Reads check custom first, then defaults. Writes target custom only.
//   Palettes in either tier may reference swatches from either tier —
//   cross-tier references are first-class.
//
// File layout (per HANDOFF "two-tier config principle"):
//   * Defaults:   ~/.config/curvz/swatches.json — loaded once at app
//     startup. Never written by the app. Curvz ships a curated set with
//     install (authoring pending before release).
//   * Custom:     <project>.curvz/swatches.json — loaded per project,
//     written on mutation with the project's existing save debounce.
//
// File I/O itself lives on the boundary — Application loads defaults
// into a seed library and hands a const reference to each CurvzProject;
// CurvzProject::save writes custom to the sibling swatches.json.
// SwatchLibrary just knows how to serialise whichever tier the caller
// asks for. See to_custom_json / from_custom_json / seed_defaults.
//
// Per-project working state (active palette, recents) stays in
// project.json's editor_state block. The library persists those via
// accessors (unchanged from Phase 4), but they are NOT part of
// to_custom_json — the swatches.json file holds swatches + palettes
// only.
//
// Phase 5 choke-point (unchanged from prior M1/M3):
//   * set_paint(obj, slot, Paint) — single write path for object paint.
//     Binding identity retention (fill_swatch_id / stroke_swatch_id on
//     SceneNode) is a separate upcoming milestone.
//   * signal_paint_changed — fires after every set_paint.
//
// Thread-safety: none. Main-thread data structure, mutated on the GTK
// main loop thread only.
//

#include "color/Palette.hpp"
#include "color/Swatch.hpp"

#include <nlohmann/json_fwd.hpp>
#include <sigc++/sigc++.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

// Forward declarations — keep SceneNode out of this header. set_paint takes
// a SceneNode& so it can read/write the object's fill/stroke, but callers
// who never touch set_paint shouldn't pay for the include.
namespace Curvz {
struct SceneNode;
}

namespace Curvz {
namespace color {

// Which side of an object's paint a site refers to. Future gradient work
// may introduce stop-level slots; extend this enum at the end (existing
// serialised values stay stable).
enum class PaintSlot {
    Fill   = 0,
    Stroke = 1,
};

} // namespace color
} // namespace Curvz

namespace Curvz {
namespace color {

class SwatchLibrary {
public:
    // Capacity of the MRU recents queue. Beyond this, the oldest entry
    // is evicted on touch_recent().
    static constexpr std::size_t RECENTS_CAPACITY = 16;

    SwatchLibrary() = default;

    // --- Swatch CRUD -------------------------------------------------------
    //
    // Writes land exclusively in the custom tier. An id that exists only
    // in defaults is effectively read-only: update_swatch / remove_swatch /
    // rename_swatch on such an id return false with a warning. The UI
    // offers duplicate-to-edit as the escape hatch (same pattern as the
    // existing Palette::builtin flag).

    // Insert a swatch into the custom tier. If the swatch's header.id is
    // empty, a fresh id is generated. If the id already exists in EITHER
    // tier, the insert is rejected and an empty string is returned.
    //
    // On success, returns the (possibly newly-generated) id.
    SwatchId add_swatch(Swatch s);

    // Replace an existing custom swatch. The new swatch's id must match
    // the `id` argument. Returns false if the id is in defaults (read-only),
    // not found, or mismatched.
    bool update_swatch(const SwatchId& id, Swatch s);

    // Remove a custom swatch AND strip it from any custom palettes,
    // recents, and favorites that reference it. Returns false if the id
    // is in defaults (read-only) or not found.
    //
    // Defaults referenced by custom palettes stay referenced — they can't
    // be removed and don't need to be.
    bool remove_swatch(const SwatchId& id);

    // Rename a custom swatch in-place. Returns false if the id is in
    // defaults (read-only) or not found.
    bool rename_swatch(const SwatchId& id, const std::string& new_name);

    // Lookup across both tiers — custom first, then defaults. Returns
    // nullptr when absent in both.
    const Swatch* find_swatch(const SwatchId& id) const;

    // Returns true iff the id refers to a swatch in the defaults tier.
    // Used by the UI to gate edit/rename/delete affordances (show
    // "duplicate to edit" instead).
    bool is_default_swatch(const SwatchId& id) const;

    // Iteration. Emits defaults first (insertion order), then custom
    // (insertion order). UI convention: shipped palette above user work.
    std::vector<SwatchId> all_swatch_ids() const;

    std::size_t swatch_count() const {
        return m_defaults_swatches.size() + m_custom_swatches.size();
    }

    // --- Palette CRUD ------------------------------------------------------
    //
    // Same tier semantics as swatches: writes land in custom; edit/delete
    // of a default-tier palette returns false. duplicate_palette(default)
    // produces a custom copy (the blessed way to "edit" a default palette).

    // Create a new palette in the custom tier. If the input's id is empty,
    // a fresh id is generated. Returns empty on id collision with either
    // tier.
    PaletteId add_palette(Palette p);

    // Remove a custom palette. Does NOT remove the swatches it referenced
    // (palettes are views, not owners). Returns false if the id is in
    // defaults (read-only) or not found.
    //
    // If the removed palette was the active one, active is cleared.
    bool remove_palette(const PaletteId& id);

    // Rename a custom palette in-place. Returns false if the id is in
    // defaults (read-only) or not found.
    bool rename_palette(const PaletteId& id, const std::string& new_name);

    // Add a swatch to a custom palette (append). Idempotent. Returns
    // false if the palette is in defaults (read-only) or builtin, or if
    // either id is unknown. The swatch id may come from EITHER tier —
    // cross-tier references are first-class.
    bool add_to_palette(const PaletteId& pid, const SwatchId& sid);

    // Remove a swatch from a custom palette. Returns false if the palette
    // is in defaults (read-only) or the swatch was not in the palette.
    bool remove_from_palette(const PaletteId& pid, const SwatchId& sid);

    // Reorder within a custom palette. Returns false if the palette is
    // in defaults (read-only) or the swatch isn't in the palette.
    bool reorder_in_palette(const PaletteId& pid, const SwatchId& sid,
                            std::size_t new_index);

    // Duplicate a palette (from EITHER tier) under a new id in the custom
    // tier. Always succeeds for an existing palette — this is how the UI
    // lets users "edit" a default palette. The copy inherits the original's
    // swatch list; cross-tier references in that list survive intact.
    // Returns the new palette id, or empty on failure.
    PaletteId duplicate_palette(const PaletteId& pid,
                                const std::string& new_name);

    // Lookup across both tiers — custom first, then defaults. Returns
    // nullptr when absent in both.
    const Palette* find_palette(const PaletteId& id) const;

    // Returns true iff the id refers to a palette in the defaults tier.
    bool is_default_palette(const PaletteId& id) const;

    // Iteration. Emits defaults first, then custom.
    std::vector<PaletteId> all_palette_ids() const;

    std::size_t palette_count() const {
        return m_defaults_palettes.size() + m_custom_palettes.size();
    }

    // --- Active palette ----------------------------------------------------
    //
    // Per-project working state. Persisted in project.json (NOT in
    // swatches.json). May point at a palette from either tier.

    const PaletteId& active_palette() const { return m_active_palette; }
    void set_active_palette(const PaletteId& id);

    // --- Recents (MRU) -----------------------------------------------------
    //
    // Per-project working state. Persisted in project.json. May contain
    // ids from either tier — clicking a default swatch counts as a recent
    // just like clicking a custom one.

    // Push this swatch to the front. Silently no-ops for unknown ids.
    void touch_recent(const SwatchId& id);

    const std::vector<SwatchId>& recents() const { return m_recents; }

    void clear_recents() { m_recents.clear(); }

    // --- Convenience for UI ------------------------------------------------

    // Find a swatch whose solid colour exactly matches. Searches CUSTOM
    // first, then defaults — user-curated names win over shipped defaults
    // with the same colour. Returns empty on no match.
    SwatchId find_solid_by_color(const Color& c) const;

    // --- Paint binding (Phase 5 M2) ----------------------------------------
    //
    // set_paint is THE choke point for object paint writes. See the prior
    // comment block for history; the two-tier refactor doesn't change the
    // contract. find_swatch inside to_fillstyle now transparently searches
    // both tiers, so a SwatchRef whose id lives in defaults resolves just
    // like a custom one.
    bool set_paint(SceneNode& obj, PaintSlot slot, const Paint& p);

    // Reads back the current paint on an object-slot. Today lifts the
    // FillStyle directly. When SceneNode gains fill_swatch_id /
    // stroke_swatch_id (next milestone), this reconstructs a SwatchRef
    // when those fields are populated.
    Paint get_paint(const SceneNode& obj, PaintSlot slot) const;

    // Emitted after every successful set_paint.
    using PaintChangedSignal = sigc::signal<void(SceneNode*, PaintSlot)>;
    PaintChangedSignal& signal_paint_changed() { return m_sig_paint_changed; }

    // Emitted after a swatch's content changes via update_swatch. Carries
    // the id so listeners (Canvas, for live-recolour in s70 M3) can filter
    // the scene walk to just the affected bindings.
    //
    // Rename fires nothing here — the id (the binding key) is immutable;
    // only the display name shifts, and bound objects don't care about
    // names. Delete also fires nothing — the remove path has its own
    // usage walk (M4).
    using SwatchChangedSignal = sigc::signal<void(const SwatchId&)>;
    SwatchChangedSignal& signal_swatch_changed() { return m_sig_swatch_changed; }

    // --- JSON round-trip ---------------------------------------------------
    //
    // Two tiers, two round-trips. Each JSON blob carries the swatches +
    // palettes pool for ONE tier — recents and active_palette are
    // per-project state and live in project.json, not in either tier's
    // swatches.json. Neither blob carries a schema_version field (per
    // decision in HANDOFF "two-tier config principle"); if a breaking
    // shape change ever happens, the absent field is treated as
    // pre-versioned.
    //
    // Defaults ownership model (option B per session s69):
    //   Application owns a master SwatchLibrary that it loads defaults
    //   into once at startup, then COPIES those defaults into every new
    //   project's library via seed_defaults(). Cheap; no shared pointers
    //   to lifetime-manage.

    // Serialise the CUSTOM tier only (swatches + palettes). Recents and
    // active_palette are NOT included — they belong to project.json.
    void to_custom_json(nlohmann::json& j) const;

    // Load the CUSTOM tier from the given json. Replaces any existing
    // custom content. Does not touch the defaults tier.
    void from_custom_json(const nlohmann::json& j);

    // Serialise the DEFAULTS tier only. Used by whatever ships or
    // generates the defaults file; the running app never calls this.
    void to_defaults_json(nlohmann::json& j) const;

    // Load the DEFAULTS tier from the given json. Replaces any existing
    // defaults content. Used by Application at startup on the master
    // library.
    void from_defaults_json(const nlohmann::json& j);

    // Copy the defaults tier from another library into this one's
    // defaults tier. Used by Application to seed each new project with
    // the master defaults pool. The custom tier of `this` is untouched.
    void seed_defaults_from(const SwatchLibrary& source);

    // --- Legacy project.json migration -------------------------------------
    //
    // Older projects (pre-s69) stored swatches/palettes/recents/active in
    // a `color_system` block inside project.json. CurvzProject::load calls
    // this on that block when the sibling swatches.json doesn't exist yet,
    // so existing projects open cleanly and migrate on next save (which
    // writes the sibling file and drops the block from project.json).
    //
    // Populates CUSTOM tier swatches + palettes, plus recents and
    // active_palette (both per-project state). Ignores the v2
    // paint_bindings block that briefly existed during s68 — nlohmann
    // skips unknown keys silently.
    void load_legacy_color_system(const nlohmann::json& j);

    // --- Introspection helpers ---------------------------------------------

    // True iff both tiers are empty — the UI's "nothing at all yet"
    // empty-state branch.
    bool empty() const {
        return m_defaults_swatches.empty() && m_custom_swatches.empty()
            && m_defaults_palettes.empty() && m_custom_palettes.empty();
    }

private:
    // Generate a fresh id that doesn't collide with anything in EITHER
    // tier of the target map kind. 122 bits of entropy — collisions are
    // not a practical concern; one defensive retry covers RNG pathology.
    template <typename MapT>
    std::string generate_unique_id(const MapT& existing_custom,
                                   const MapT& existing_defaults) const;

    // Two pools per kind. Iteration in both is insertion-order (std::map)
    // so UI display and save-file diffs are stable.
    std::map<SwatchId,  Swatch>  m_defaults_swatches;
    std::map<SwatchId,  Swatch>  m_custom_swatches;
    std::map<PaletteId, Palette> m_defaults_palettes;
    std::map<PaletteId, Palette> m_custom_palettes;

    // Per-project working state. Always persists to project.json.
    std::vector<SwatchId> m_recents;
    PaletteId             m_active_palette;

    PaintChangedSignal m_sig_paint_changed;
    SwatchChangedSignal m_sig_swatch_changed;
};

// ── App-global defaults singleton ───────────────────────────────────────────
//
// The application loads defaults from ~/.config/curvz/swatches.json once at
// startup via load_app_defaults() and stashes them here. CurvzProject::load
// and CurvzProject::create_* call seed_defaults_from(swatch_defaults()) on
// each new project library to populate the defaults tier.
//
// Function-local static — same pattern as MacroManager::instance(). Decoupled
// from Application's lifecycle; safe to call from anywhere that includes
// this header after startup.
//
// If the defaults file doesn't exist or fails to parse, the singleton is
// simply empty — the two-tier plumbing works fine with an empty defaults
// tier (custom tier holds everything the user sees, same as Phase 4).

SwatchLibrary& swatch_defaults();

// Load the app-global defaults from disk into the singleton. Idempotent —
// subsequent calls replace the singleton's defaults pool. Called once from
// Application::on_activate().
//
// Reads ~/.config/curvz/swatches.json via Glib::get_user_config_dir(). A
// missing file is not an error — the function returns false with an info
// log and the singleton stays empty. A malformed file IS an error — the
// function returns false with a warning and leaves the singleton in
// whatever state the partial parse left it (typically empty).
bool load_app_defaults();

// Path where the app-global defaults file is expected to live. Exposed for
// logging and for future "write a seed defaults file" tooling (post-1.0).
std::string app_defaults_path();

} // namespace color
} // namespace Curvz
