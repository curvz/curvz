//
// ThemeLibrary.cpp — two-list registry implementation (S103 m1).
//
// Mirrors StyleLibrary.cpp shape exactly. App-tier list is empty in v1;
// user-tier vector is mutated through CRUD methods, signals fire on
// every successful write.
//
// JSON round-trip pumps (theme_to_json / theme_from_json) live at
// namespace theme:: scope so the m2 ThemeIO bridge can reuse the exact
// shape used by the project-tier round-trip — same lift-out-of-anon-
// namespace move that S102 m1 made for style_to_json / style_from_json.
//
// Apply funnel (capture_theme_from_doc / apply_theme_to_doc) lives at
// the bottom of this file rather than in a separate translation unit.
// The two free functions are short, share no helpers with anything else,
// and putting them here keeps "the entire Theme System core" in one
// translation unit. Move out if it grows.
//

#include "theme/ThemeLibrary.hpp"
#include "SceneNode.hpp"        // generate_internal_id() — GLib UUID v4
#include "CurvzDocument.hpp"    // CurvzDocument, ensure_*_layer
#include "UnitSystem.hpp"       // UnitSystem::label / parse_unit
#include "CurvzLog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace Curvz {
namespace theme {

// ── Constructor ─────────────────────────────────────────────────────────────

ThemeLibrary::ThemeLibrary() {
    // No app stub list — v1 has no built-in themes. The two-list
    // architecture is preserved for symmetry with StyleLibrary; future
    // app themes seed in here.
    LOG_DEBUG("ThemeLibrary: constructed (0 app, 0 user)");
}

// ── id generation ───────────────────────────────────────────────────────────

ThemeId ThemeLibrary::generate_unique_user_id() const {
    // Same UUID source as StyleLibrary and SwatchLibrary. "thm_" prefix
    // matches the Theme System's naming convention.
    for (int attempt = 0; attempt < 2; ++attempt) {
        ThemeId candidate = "thm_" + generate_internal_id();
        if (find_user(candidate) == m_user_themes.end() &&
            find_app(candidate)  == m_app_themes.end()) {
            return candidate;
        }
    }
    LOG_ERROR("ThemeLibrary::generate_unique_user_id: 2 consecutive UUID "
              "collisions (user {}, app {}) — RNG failure?",
              m_user_themes.size(), m_app_themes.size());
    return {};
}

// ── Internal lookups ────────────────────────────────────────────────────────

std::vector<Theme>::iterator ThemeLibrary::find_user(const ThemeId& id) {
    return std::find_if(m_user_themes.begin(), m_user_themes.end(),
                        [&](const Theme& t) { return t.header.id == id; });
}

std::vector<Theme>::const_iterator ThemeLibrary::find_user(const ThemeId& id) const {
    return std::find_if(m_user_themes.begin(), m_user_themes.end(),
                        [&](const Theme& t) { return t.header.id == id; });
}

std::vector<Theme>::const_iterator ThemeLibrary::find_app(const ThemeId& id) const {
    return std::find_if(m_app_themes.begin(), m_app_themes.end(),
                        [&](const Theme& t) { return t.header.id == id; });
}

// ── User CRUD ───────────────────────────────────────────────────────────────

ThemeId ThemeLibrary::add_theme(Theme t) {
    if (t.header.id.empty()) {
        t.header.id = generate_unique_user_id();
        if (t.header.id.empty()) return {};
    } else {
        if (theme::is_built_in(t.header.id)) {
            LOG_WARN("ThemeLibrary::add_theme: refusing 'app:' id '{}' on add",
                     t.header.id);
            return {};
        }
        if (find_user(t.header.id) != m_user_themes.end() ||
            find_app(t.header.id)  != m_app_themes.end()) {
            LOG_WARN("ThemeLibrary::add_theme: id '{}' already exists, refusing",
                     t.header.id);
            return {};
        }
    }
    ThemeId id = t.header.id;
    m_user_themes.push_back(std::move(t));
    m_sig_added.emit(id);
    return id;
}

bool ThemeLibrary::update_theme(const ThemeId& id, Theme t) {
    if (theme::is_built_in(id)) {
        LOG_WARN("ThemeLibrary::update_theme: '{}' is built-in (read-only)", id);
        return false;
    }
    if (t.header.id != id) {
        LOG_WARN("ThemeLibrary::update_theme: header.id mismatch ('{}' vs '{}')",
                 t.header.id, id);
        return false;
    }
    auto it = find_user(id);
    if (it == m_user_themes.end()) {
        LOG_WARN("ThemeLibrary::update_theme: id '{}' not found in user list", id);
        return false;
    }
    *it = std::move(t);
    m_sig_changed.emit(id);
    return true;
}

bool ThemeLibrary::remove_theme(const ThemeId& id) {
    if (theme::is_built_in(id)) {
        LOG_WARN("ThemeLibrary::remove_theme: '{}' is built-in (read-only)", id);
        return false;
    }
    auto it = find_user(id);
    if (it == m_user_themes.end()) {
        LOG_WARN("ThemeLibrary::remove_theme: id '{}' not found in user list", id);
        return false;
    }
    m_user_themes.erase(it);
    m_sig_removed.emit(id);
    return true;
}

ThemeId ThemeLibrary::duplicate_to_user(const ThemeId& src) {
    const Theme* p = find_theme(src);
    if (!p) {
        LOG_WARN("ThemeLibrary::duplicate_to_user: id '{}' not found", src);
        return {};
    }
    Theme copy = *p;
    copy.header.id = generate_unique_user_id();
    if (copy.header.id.empty()) return {};
    if (!copy.header.name.empty()) copy.header.name += " copy";
    ThemeId id = copy.header.id;
    m_user_themes.push_back(std::move(copy));
    m_sig_added.emit(id);
    return id;
}

// ── Lookup ──────────────────────────────────────────────────────────────────

const Theme* ThemeLibrary::find_theme(const ThemeId& id) const {
    if (auto it = find_user(id); it != m_user_themes.end()) return &*it;
    if (auto it = find_app(id);  it != m_app_themes.end())  return &*it;
    return nullptr;
}

bool ThemeLibrary::is_built_in(const ThemeId& id) const {
    // Tighter than the free predicate — actually checks the app list.
    // v1 always returns false (app list empty).
    return find_app(id) != m_app_themes.end();
}

// ── Category accessors ──────────────────────────────────────────────────────

std::vector<const Theme*>
ThemeLibrary::app_themes_in_category(const std::string& cat) const {
    std::vector<const Theme*> out;
    for (const Theme& t : m_app_themes) {
        if (t.header.category == cat) out.push_back(&t);
    }
    return out;
}

std::vector<const Theme*>
ThemeLibrary::user_themes_in_category(const std::string& cat) const {
    std::vector<const Theme*> out;
    for (const Theme& t : m_user_themes) {
        if (t.header.category == cat) out.push_back(&t);
    }
    return out;
}

std::vector<std::string> ThemeLibrary::user_categories() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const Theme& t : m_user_themes) {
        if (seen.insert(t.header.category).second) {
            out.push_back(t.header.category);
        }
    }
    return out;
}

std::vector<std::string> ThemeLibrary::app_categories() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const Theme& t : m_app_themes) {
        if (seen.insert(t.header.category).second) {
            out.push_back(t.header.category);
        }
    }
    return out;
}

// ── Name uniqueness query ───────────────────────────────────────────────────

bool ThemeLibrary::has_user_name(const std::string& name) const {
    for (const Theme& t : m_user_themes) {
        if (t.header.name == name) return true;
    }
    return false;
}

// ── JSON round-trip ─────────────────────────────────────────────────────────
//
// Per-entry shape:
//   {
//     "header":     {"id": "thm_...", "name": "...", "category": "..."},
//     "units":      {"display_unit": "px"|"in"|"mm"|"pt"},
//     "background": {"artboard_r": ..., ...},
//     "guides":     {"color_r": ..., ..., "visible": true},
//     "grid":       {"enabled": ..., "spacing_x": ..., ...},
//     "margins":    {"enabled": ..., "top": ..., ...},
//     "snap":       {"enabled": ..., "snap_guides": ..., ...}
//   }
//
// File-scope helpers sit in the anonymous namespace below; the per-Theme
// pumps live at namespace theme:: scope so the m2 ThemeIO bridge can
// reuse them without a re-declaration. Same lift-out-of-anon move that
// S102 m1 made for style_to_json.

namespace {

using json = nlohmann::json;

// Convert Unit <-> string. UnitSystem has these but not exactly in this
// shape — its `parse_unit` returns Unit::Px for unrecognised tokens
// without logging. We want a logged warn here so a corrupt project.json
// doesn't silently swallow a bad unit.

json units_to_json(const UnitSettings& u) {
    return json{
        {"display_unit", UnitSystem::label(u.display_unit)},
    };
}

UnitSettings units_from_json(const json& j) {
    UnitSettings u;
    if (!j.is_object()) return u;
    std::string s = j.value("display_unit", std::string{"px"});
    // UnitSystem::parse_unit silently returns Px for unknown tokens; we
    // want a logged warning if an unexpected string lands here.
    if (s == "px")      u.display_unit = Unit::Px;
    else if (s == "in") u.display_unit = Unit::In;
    else if (s == "mm") u.display_unit = Unit::Mm;
    else if (s == "pt") u.display_unit = Unit::Pt;
    else {
        LOG_WARN("ThemeLibrary JSON: unknown unit '{}', using px", s);
        u.display_unit = Unit::Px;
    }
    return u;
}

// bg_to_json / bg_from_json removed in s116 m6. Replaced in s149 m1 by
// motif_to_json / motif_from_json below — the new sub-bundle carries
// both Dark and Light triples (artboard / workspace / creation), where
// the old single triple lived on the project. Old theme JSON files
// carrying a "background" key still load fine: the read site below
// silently ignores unknown keys.

json motif_to_json(const MotifSettings& m) {
    return json{
        {"dark_artboard_r",  m.dark_artboard_r},
        {"dark_artboard_g",  m.dark_artboard_g},
        {"dark_artboard_b",  m.dark_artboard_b},
        {"dark_workspace_r", m.dark_workspace_r},
        {"dark_workspace_g", m.dark_workspace_g},
        {"dark_workspace_b", m.dark_workspace_b},
        {"dark_creation_r",  m.dark_creation_r},
        {"dark_creation_g",  m.dark_creation_g},
        {"dark_creation_b",  m.dark_creation_b},
        {"light_artboard_r",  m.light_artboard_r},
        {"light_artboard_g",  m.light_artboard_g},
        {"light_artboard_b",  m.light_artboard_b},
        {"light_workspace_r", m.light_workspace_r},
        {"light_workspace_g", m.light_workspace_g},
        {"light_workspace_b", m.light_workspace_b},
        {"light_creation_r",  m.light_creation_r},
        {"light_creation_g",  m.light_creation_g},
        {"light_creation_b",  m.light_creation_b},
    };
}

MotifSettings motif_from_json(const json& j) {
    MotifSettings m;
    if (!j.is_object()) return m;
    m.dark_artboard_r  = j.value("dark_artboard_r",  m.dark_artboard_r);
    m.dark_artboard_g  = j.value("dark_artboard_g",  m.dark_artboard_g);
    m.dark_artboard_b  = j.value("dark_artboard_b",  m.dark_artboard_b);
    m.dark_workspace_r = j.value("dark_workspace_r", m.dark_workspace_r);
    m.dark_workspace_g = j.value("dark_workspace_g", m.dark_workspace_g);
    m.dark_workspace_b = j.value("dark_workspace_b", m.dark_workspace_b);
    m.dark_creation_r  = j.value("dark_creation_r",  m.dark_creation_r);
    m.dark_creation_g  = j.value("dark_creation_g",  m.dark_creation_g);
    m.dark_creation_b  = j.value("dark_creation_b",  m.dark_creation_b);
    m.light_artboard_r  = j.value("light_artboard_r",  m.light_artboard_r);
    m.light_artboard_g  = j.value("light_artboard_g",  m.light_artboard_g);
    m.light_artboard_b  = j.value("light_artboard_b",  m.light_artboard_b);
    m.light_workspace_r = j.value("light_workspace_r", m.light_workspace_r);
    m.light_workspace_g = j.value("light_workspace_g", m.light_workspace_g);
    m.light_workspace_b = j.value("light_workspace_b", m.light_workspace_b);
    m.light_creation_r  = j.value("light_creation_r",  m.light_creation_r);
    m.light_creation_g  = j.value("light_creation_g",  m.light_creation_g);
    m.light_creation_b  = j.value("light_creation_b",  m.light_creation_b);
    return m;
}

json guides_to_json(const GuideSettings& g) {
    return json{
        {"color_r", g.color_r},
        {"color_g", g.color_g},
        {"color_b", g.color_b},
        {"visible", g.visible},
    };
}

GuideSettings guides_from_json(const json& j) {
    GuideSettings g;
    if (!j.is_object()) return g;
    g.color_r = j.value("color_r", g.color_r);
    g.color_g = j.value("color_g", g.color_g);
    g.color_b = j.value("color_b", g.color_b);
    g.visible = j.value("visible", g.visible);
    return g;
}

json grid_to_json(const GridSettings& g) {
    return json{
        {"enabled",   g.enabled},
        {"visible",   g.visible},
        {"spacing_x", g.spacing_x},
        {"spacing_y", g.spacing_y},
        {"offset_x",  g.offset_x},
        {"offset_y",  g.offset_y},
        {"color_r",   g.color_r},
        {"color_g",   g.color_g},
        {"color_b",   g.color_b},
        {"color_a",   g.color_a},
        {"dots",      g.dots},
    };
}

GridSettings grid_from_json(const json& j) {
    GridSettings g;
    if (!j.is_object()) return g;
    g.enabled   = j.value("enabled",   g.enabled);
    g.visible   = j.value("visible",   g.visible);
    g.spacing_x = j.value("spacing_x", g.spacing_x);
    g.spacing_y = j.value("spacing_y", g.spacing_y);
    g.offset_x  = j.value("offset_x",  g.offset_x);
    g.offset_y  = j.value("offset_y",  g.offset_y);
    g.color_r   = j.value("color_r",   g.color_r);
    g.color_g   = j.value("color_g",   g.color_g);
    g.color_b   = j.value("color_b",   g.color_b);
    g.color_a   = j.value("color_a",   g.color_a);
    g.dots      = j.value("dots",      g.dots);
    return g;
}

json margins_to_json(const MarginSettings& m) {
    return json{
        {"enabled", m.enabled},
        {"visible", m.visible},
        {"top",     m.top},
        {"bottom",  m.bottom},
        {"left",    m.left},
        {"right",   m.right},
        {"columns", m.columns},
        {"col_gap", m.col_gap},
        {"rows",    m.rows},
        {"row_gap", m.row_gap},
        {"color_r", m.color_r},
        {"color_g", m.color_g},
        {"color_b", m.color_b},
        {"color_a", m.color_a},
    };
}

MarginSettings margins_from_json(const json& j) {
    MarginSettings m;
    if (!j.is_object()) return m;
    m.enabled = j.value("enabled", m.enabled);
    m.visible = j.value("visible", m.visible);
    m.top     = j.value("top",     m.top);
    m.bottom  = j.value("bottom",  m.bottom);
    m.left    = j.value("left",    m.left);
    m.right   = j.value("right",   m.right);
    m.columns = j.value("columns", m.columns);
    m.col_gap = j.value("col_gap", m.col_gap);
    m.rows    = j.value("rows",    m.rows);
    m.row_gap = j.value("row_gap", m.row_gap);
    m.color_r = j.value("color_r", m.color_r);
    m.color_g = j.value("color_g", m.color_g);
    m.color_b = j.value("color_b", m.color_b);
    m.color_a = j.value("color_a", m.color_a);
    return m;
}

json snap_to_json(const ThemeSnapSettings& s) {
    return json{
        {"enabled",      s.enabled},
        {"snap_guides",  s.snap_guides},
        {"snap_grid",    s.snap_grid},
        {"snap_margins", s.snap_margins},
        {"snap_nodes",   s.snap_nodes},
        {"snap_edges",   s.snap_edges},
        {"snap_centers", s.snap_centers},
    };
}

ThemeSnapSettings snap_from_json(const json& j) {
    ThemeSnapSettings s;
    if (!j.is_object()) return s;
    s.enabled      = j.value("enabled",      s.enabled);
    s.snap_guides  = j.value("snap_guides",  s.snap_guides);
    s.snap_grid    = j.value("snap_grid",    s.snap_grid);
    s.snap_margins = j.value("snap_margins", s.snap_margins);
    s.snap_nodes   = j.value("snap_nodes",   s.snap_nodes);
    s.snap_edges   = j.value("snap_edges",   s.snap_edges);
    s.snap_centers = j.value("snap_centers", s.snap_centers);
    return s;
}

} // anon namespace — the per-Theme pumps below live at namespace theme::
//                   scope so the m2 ThemeIO bridge can call them.

json theme_to_json(const Theme& t) {
    json entry;
    entry["header"] = {
        {"id",       t.header.id},
        {"name",     t.header.name},
        {"category", t.header.category}
    };
    entry["units"]      = units_to_json(t.units);
    entry["motif"]      = motif_to_json(t.motif);   // s149 m1
    entry["guides"]     = guides_to_json(t.guides);
    entry["grid"]       = grid_to_json(t.grid);
    entry["margins"]    = margins_to_json(t.margins);
    entry["snap"]       = snap_to_json(t.snap);

    return entry;
}

std::optional<Theme> theme_from_json(const json& entry) {
    if (!entry.is_object()) {
        LOG_WARN("ThemeLibrary JSON: theme entry not an object, skipping");
        return std::nullopt;
    }

    Theme t;

    if (entry.contains("header") && entry["header"].is_object()) {
        const auto& h = entry["header"];
        t.header.id       = h.value("id",       std::string{});
        t.header.name     = h.value("name",     std::string{});
        t.header.category = h.value("category", std::string{});
    }
    if (t.header.id.empty()) {
        LOG_WARN("ThemeLibrary JSON: theme entry missing header.id, skipping");
        return std::nullopt;
    }
    if (theme::is_built_in(t.header.id)) {
        // Defensive: 'app:' ids belong only to a hardcoded list (none in
        // v1). Drop on load — same posture as StyleLibrary.
        LOG_WARN("ThemeLibrary JSON: refusing 'app:' id '{}' in user tier, "
                 "skipping", t.header.id);
        return std::nullopt;
    }

    // Each sub-block is permissively defaulted when missing — this is
    // how older project.json files written before some sub-block existed
    // load with the new fields at struct-default values.
    if (entry.contains("units"))      t.units      = units_from_json(entry["units"]);
    // s149 m1: motif sub-block. Pre-s149 themes have no "motif" key —
    // the struct defaults supply a sensible Dark + Light pair so apply
    // doesn't write zeros into the doc. Old "background" sub-block
    // (s116-pre) is silently ignored if encountered.
    if (entry.contains("motif"))      t.motif      = motif_from_json(entry["motif"]);
    if (entry.contains("guides"))     t.guides     = guides_from_json(entry["guides"]);
    if (entry.contains("grid"))       t.grid       = grid_from_json(entry["grid"]);
    if (entry.contains("margins"))    t.margins    = margins_from_json(entry["margins"]);
    if (entry.contains("snap"))       t.snap       = snap_from_json(entry["snap"]);

    return t;
}

void ThemeLibrary::to_user_json(json& j) const {
    json arr = json::array();
    for (const Theme& t : m_user_themes) {
        arr.push_back(theme_to_json(t));
    }
    j["themes"] = std::move(arr);
}

void ThemeLibrary::from_user_json(const json& j) {
    // Atomic replace, no signals. CurvzProject::load is expected to
    // follow up with whatever redraw / refresh dance applies.
    m_user_themes.clear();

    if (!j.contains("themes") || !j["themes"].is_array()) {
        LOG_INFO("ThemeLibrary::from_user_json: no 'themes' array, user tier "
                 "left empty");
        return;
    }

    std::size_t skipped = 0;
    for (const auto& entry : j["themes"]) {
        auto t = theme_from_json(entry);
        if (!t) { ++skipped; continue; }
        if (find_user(t->header.id) != m_user_themes.end() ||
            find_app(t->header.id)  != m_app_themes.end()) {
            LOG_WARN("ThemeLibrary::from_user_json: id '{}' collides, skipping",
                     t->header.id);
            ++skipped;
            continue;
        }
        m_user_themes.push_back(std::move(*t));
    }

    if (skipped > 0) {
        LOG_INFO("ThemeLibrary::from_user_json: loaded {} user theme(s), "
                 "skipped {}", m_user_themes.size(), skipped);
    } else {
        LOG_INFO("ThemeLibrary::from_user_json: loaded {} user theme(s)",
                 m_user_themes.size());
    }
}

// ── Capture / Apply (free functions) ────────────────────────────────────────
//
// These are the only Theme-System callers that touch CurvzDocument; the
// rest of the file is library bookkeeping. Living at the bottom keeps
// the dependency clear: ThemeLibrary itself is pure data; the bridge
// to documents is named, scoped, and easy to find.

Theme capture_theme_from_doc(const CurvzDocument& doc, Motif current_motif) {
    Theme t;
    // header is left empty — caller (the m3 dialog's "Save current as
    // theme…" path, or the dual-source apply path that uses this as a
    // transient bundle) fills name/category as appropriate.

    t.units.display_unit = doc.canvas.display_unit;

    // s149 m1: motif sub-bundle. The doc carries one triple at a time
    // (the active mode's). Capture writes those values into the matching
    // slot of MotifSettings; the off-mode slot keeps its struct-default
    // factory values so apply-in-the-other-mode produces a sensible
    // result rather than the wrong-mode tone. The user can re-capture
    // in the other mode to overwrite the off-mode slot if they want a
    // custom Light look paired with their custom Dark look.
    if (current_motif == Motif::Light) {
        t.motif.light_artboard_r  = doc.artboard_bg_r;
        t.motif.light_artboard_g  = doc.artboard_bg_g;
        t.motif.light_artboard_b  = doc.artboard_bg_b;
        t.motif.light_workspace_r = doc.workspace_bg_r;
        t.motif.light_workspace_g = doc.workspace_bg_g;
        t.motif.light_workspace_b = doc.workspace_bg_b;
        t.motif.light_creation_r  = doc.creation_color_r;
        t.motif.light_creation_g  = doc.creation_color_g;
        t.motif.light_creation_b  = doc.creation_color_b;
        // dark_* slots stay at MotifSettings struct defaults.
    } else {
        t.motif.dark_artboard_r  = doc.artboard_bg_r;
        t.motif.dark_artboard_g  = doc.artboard_bg_g;
        t.motif.dark_artboard_b  = doc.artboard_bg_b;
        t.motif.dark_workspace_r = doc.workspace_bg_r;
        t.motif.dark_workspace_g = doc.workspace_bg_g;
        t.motif.dark_workspace_b = doc.workspace_bg_b;
        t.motif.dark_creation_r  = doc.creation_color_r;
        t.motif.dark_creation_g  = doc.creation_color_g;
        t.motif.dark_creation_b  = doc.creation_color_b;
        // light_* slots stay at MotifSettings struct defaults.
    }

    t.guides.color_r = doc.guide_color_r;
    t.guides.color_g = doc.guide_color_g;
    t.guides.color_b = doc.guide_color_b;
    // Visibility lives on the GuideLayer SceneNode. Read-through-pointer
    // — if the layer doesn't exist, default to visible=true so a
    // re-apply on a fresh doc doesn't silently hide an as-yet-uncreated
    // layer.
    if (const SceneNode* gl = doc.guide_layer()) {
        t.guides.visible = gl->visible;
    }

    if (const SceneNode* gl = doc.grid_layer()) {
        t.grid.enabled   = true;
        t.grid.visible   = gl->visible;
        t.grid.spacing_x = gl->grid_spacing_x;
        t.grid.spacing_y = gl->grid_spacing_y;
        t.grid.offset_x  = gl->grid_offset_x;
        t.grid.offset_y  = gl->grid_offset_y;
        t.grid.color_r   = gl->grid_color_r;
        t.grid.color_g   = gl->grid_color_g;
        t.grid.color_b   = gl->grid_color_b;
        t.grid.color_a   = gl->grid_color_a;
        t.grid.dots      = gl->grid_dots;
    }
    // else: t.grid stays at struct defaults with enabled=false. Apply
    // will read enabled=false and remove any GridLayer in the target.

    if (const SceneNode* ml = doc.margin_layer()) {
        t.margins.enabled = true;
        t.margins.visible = ml->visible;
        t.margins.top     = ml->margin_top;
        t.margins.bottom  = ml->margin_bottom;
        t.margins.left    = ml->margin_left;
        t.margins.right   = ml->margin_right;
        t.margins.columns = ml->margin_columns;
        t.margins.col_gap = ml->margin_col_gap;
        t.margins.rows    = ml->margin_rows;
        t.margins.row_gap = ml->margin_row_gap;
        t.margins.color_r = ml->margin_color_r;
        t.margins.color_g = ml->margin_color_g;
        t.margins.color_b = ml->margin_color_b;
        t.margins.color_a = ml->margin_color_a;
    }

    t.snap = doc.snap;

    return t;
}

void apply_theme_to_doc(const Theme& theme, CurvzDocument& doc,
                        Motif current_motif) {
    // ── Order matters ─────────────────────────────────────────────────
    //
    // Layer-presence changes go FIRST so subsequent per-layer field
    // writes see the layers they expect. Doc-level fields (units,
    // background colours, guide colour, snap struct) have no layer
    // dependency and can run in any order; we run them after for
    // grouping consistency, but it's not load-bearing.

    // ── Grid: ensure-or-remove ─────────────────────────────────────────
    if (theme.grid.enabled) {
        SceneNode* gl = doc.ensure_grid_layer();
        if (gl) {
            gl->visible        = theme.grid.visible;
            gl->grid_spacing_x = theme.grid.spacing_x;
            gl->grid_spacing_y = theme.grid.spacing_y;
            gl->grid_offset_x  = theme.grid.offset_x;
            gl->grid_offset_y  = theme.grid.offset_y;
            gl->grid_color_r   = theme.grid.color_r;
            gl->grid_color_g   = theme.grid.color_g;
            gl->grid_color_b   = theme.grid.color_b;
            gl->grid_color_a   = theme.grid.color_a;
            gl->grid_dots      = theme.grid.dots;
        }
    } else {
        // Remove any existing GridLayer. Same erase predicate the
        // PropertiesPanel uses when the user toggles Grid.Enable off.
        auto& layers = doc.layers;
        layers.erase(
            std::remove_if(layers.begin(), layers.end(),
                [](const std::unique_ptr<SceneNode>& l) {
                    return l && l->is_grid_layer();
                }),
            layers.end());
    }

    // ── Margins: ensure-or-remove ──────────────────────────────────────
    if (theme.margins.enabled) {
        SceneNode* ml = doc.ensure_margin_layer();
        if (ml) {
            ml->visible        = theme.margins.visible;
            ml->margin_top     = theme.margins.top;
            ml->margin_bottom  = theme.margins.bottom;
            ml->margin_left    = theme.margins.left;
            ml->margin_right   = theme.margins.right;
            ml->margin_columns = theme.margins.columns;
            ml->margin_col_gap = theme.margins.col_gap;
            ml->margin_rows    = theme.margins.rows;
            ml->margin_row_gap = theme.margins.row_gap;
            ml->margin_color_r = theme.margins.color_r;
            ml->margin_color_g = theme.margins.color_g;
            ml->margin_color_b = theme.margins.color_b;
            ml->margin_color_a = theme.margins.color_a;
        }
    } else {
        auto& layers = doc.layers;
        layers.erase(
            std::remove_if(layers.begin(), layers.end(),
                [](const std::unique_ptr<SceneNode>& l) {
                    return l && l->is_margin_layer();
                }),
            layers.end());
    }

    // ── Doc-level fields (no layer dependency) ─────────────────────────

    doc.canvas.display_unit = theme.units.display_unit;

    // s149 m1: motif sub-bundle. The doc carries a single triple of
    // artboard/workspace/creation values; the theme carries both Dark
    // and Light pairs. Write the matching pair for the user's current
    // appearance mode. The off-mode pair is preserved in the theme but
    // not written here — flipping appearance mode later doesn't re-run
    // apply, so a doc currently in Dark gets the theme's dark_* values
    // and stays with those even if the user later switches to Light.
    // Switching appearance modes is a workspace-wide CSS flip; per-doc
    // colour pairs are independent of it (see the s148 m1 demotion
    // rationale on CurvzDocument::artboard_bg_*).
    if (current_motif == Motif::Light) {
        doc.artboard_bg_r  = theme.motif.light_artboard_r;
        doc.artboard_bg_g  = theme.motif.light_artboard_g;
        doc.artboard_bg_b  = theme.motif.light_artboard_b;
        doc.workspace_bg_r = theme.motif.light_workspace_r;
        doc.workspace_bg_g = theme.motif.light_workspace_g;
        doc.workspace_bg_b = theme.motif.light_workspace_b;
        doc.creation_color_r = theme.motif.light_creation_r;
        doc.creation_color_g = theme.motif.light_creation_g;
        doc.creation_color_b = theme.motif.light_creation_b;
    } else {
        doc.artboard_bg_r  = theme.motif.dark_artboard_r;
        doc.artboard_bg_g  = theme.motif.dark_artboard_g;
        doc.artboard_bg_b  = theme.motif.dark_artboard_b;
        doc.workspace_bg_r = theme.motif.dark_workspace_r;
        doc.workspace_bg_g = theme.motif.dark_workspace_g;
        doc.workspace_bg_b = theme.motif.dark_workspace_b;
        doc.creation_color_r = theme.motif.dark_creation_r;
        doc.creation_color_g = theme.motif.dark_creation_g;
        doc.creation_color_b = theme.motif.dark_creation_b;
    }

    doc.guide_color_r = theme.guides.color_r;
    doc.guide_color_g = theme.guides.color_g;
    doc.guide_color_b = theme.guides.color_b;
    // Guides layer visibility — apply only if a GuideLayer exists. We
    // don't synthesise one (a doc without guides shouldn't have a
    // hidden GuideLayer materialise out of the apply); existing layers
    // get the visibility flag updated.
    if (SceneNode* gl = doc.guide_layer()) {
        gl->visible = theme.guides.visible;
    }

    doc.snap = theme.snap;
    // The project-wide doc.snap mirror lives on CurvzProject::snap (set
    // by the Toolbar's snap switch + popover handlers; see s150 — the
    // inspector Snap section was deleted, Toolbar is now the canonical
    // writer). The apply funnel doesn't have a project pointer; the
    // dialog driver in m3 is responsible for syncing the project mirror
    // after applying to the active doc, same way the existing snap edit
    // paths do.
}

} // namespace theme
} // namespace Curvz
