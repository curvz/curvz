//
// StyleLibrary.cpp — two-list registry implementation (Phase 1 m1).
//
// App styles: hardcoded stub list of 3 entries, populated at construction.
// User styles: vector, mutated through CRUD methods, signals fire on every
// successful write.
//
// The CRUD invariant is uniform: every write path consults is_built_in()
// (the free predicate from Style.hpp) on the input id before touching
// anything. App ids are read-only; the escape hatch is duplicate_to_user.
//
// JSON round-trip is NOT in m1 — user styles persist for the lifetime of
// the loaded project only. m3 wires up serialisation alongside the
// SVG round-trip and SceneNode bound_style work.
//

#include "style/StyleLibrary.hpp"
#include "SceneNode.hpp"   // generate_internal_id() — GLib UUID v4
#include "CurvzLog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace Curvz {
namespace style {

// ── App-style stub list ─────────────────────────────────────────────────────
//
// Three entries to validate the two-list shape and exercise the panel
// (Phase 2). Curated content is Phase 2 work — these are deliberately
// skeletal placeholders, not the user-facing v1.0 set.
//
//   app:default       — black fill, no stroke. The "boring SVG default"
//                       baseline; useful as a reset target.
//   app:outline-only  — no fill, 1px black stroke. The mirror image of
//                       app:default; common UI-icon shape.
//   app:fill-only     — black fill, no stroke. Same render as default
//                       but distinct id, used by the icon export pipeline
//                       to tag "treat as monochrome glyph". Categories
//                       differ to verify the panel groups correctly.
//
// These are skeletons; the real content stack lands in Phase 2.

static Style make_app_default() {
    Style s;
    s.header.id       = "app:default";
    s.header.name     = "Default";
    s.header.category = "Built-in";
    s.fill            = color::Solid{color::Color::black()};
    s.stroke.paint    = color::None{};
    return s;
}

static Style make_app_outline_only() {
    Style s;
    s.header.id          = "app:outline-only";
    s.header.name        = "Outline Only";
    s.header.category    = "Built-in";
    s.fill               = color::None{};
    s.stroke.paint       = color::Solid{color::Color::black()};
    s.stroke.width       = 1.0;
    return s;
}

static Style make_app_fill_only() {
    Style s;
    s.header.id          = "app:fill-only";
    s.header.name        = "Fill Only";
    s.header.category    = "Built-in";
    s.fill               = color::Solid{color::Color::black()};
    s.stroke.paint       = color::None{};
    return s;
}

// ── Constructor ─────────────────────────────────────────────────────────────

StyleLibrary::StyleLibrary() {
    m_app_styles.push_back(make_app_default());
    m_app_styles.push_back(make_app_outline_only());
    m_app_styles.push_back(make_app_fill_only());
    LOG_DEBUG("StyleLibrary: constructed with {} app stubs, 0 user styles",
              m_app_styles.size());
}

// ── id generation ───────────────────────────────────────────────────────────

StyleId StyleLibrary::generate_unique_user_id() const {
    // Same UUID source as SceneNode and SwatchLibrary. Prefix with "stl_"
    // so the id discriminator (is_built_in) cleanly separates user from
    // app — and also so the SVG round-trip can spot user-style refs at
    // a glance during debug.
    for (int attempt = 0; attempt < 2; ++attempt) {
        StyleId candidate = "stl_" + generate_internal_id();
        if (find_user(candidate) == m_user_styles.end() &&
            find_app(candidate)  == m_app_styles.end()) {
            return candidate;
        }
    }
    LOG_ERROR("StyleLibrary::generate_unique_user_id: 2 consecutive UUID "
              "collisions (user {}, app {}) — RNG failure?",
              m_user_styles.size(), m_app_styles.size());
    return {};
}

// ── Internal lookups ────────────────────────────────────────────────────────

std::vector<Style>::iterator StyleLibrary::find_user(const StyleId& id) {
    return std::find_if(m_user_styles.begin(), m_user_styles.end(),
                        [&](const Style& s) { return s.header.id == id; });
}

std::vector<Style>::const_iterator StyleLibrary::find_user(const StyleId& id) const {
    return std::find_if(m_user_styles.begin(), m_user_styles.end(),
                        [&](const Style& s) { return s.header.id == id; });
}

std::vector<Style>::const_iterator StyleLibrary::find_app(const StyleId& id) const {
    return std::find_if(m_app_styles.begin(), m_app_styles.end(),
                        [&](const Style& s) { return s.header.id == id; });
}

// ── User CRUD ───────────────────────────────────────────────────────────────

StyleId StyleLibrary::add_style(Style s) {
    // Generate-or-validate the id.
    if (s.header.id.empty()) {
        s.header.id = generate_unique_user_id();
        if (s.header.id.empty()) return {};  // RNG pathology, logged above
    } else {
        // App-prefixed ids can't be added — the user list is for "stl_"
        // ids only, and app entries are owned by the constructor.
        if (style::is_built_in(s.header.id)) {
            LOG_WARN("StyleLibrary::add_style: refusing 'app:' id '{}' on add",
                     s.header.id);
            return {};
        }
        // No collisions in either list.
        if (find_user(s.header.id) != m_user_styles.end() ||
            find_app(s.header.id)  != m_app_styles.end()) {
            LOG_WARN("StyleLibrary::add_style: id '{}' already exists, refusing",
                     s.header.id);
            return {};
        }
    }
    StyleId id = s.header.id;
    m_user_styles.push_back(std::move(s));
    m_sig_added.emit(id);
    return id;
}

bool StyleLibrary::update_style(const StyleId& id, Style s) {
    if (style::is_built_in(id)) {
        LOG_WARN("StyleLibrary::update_style: '{}' is built-in (read-only)", id);
        return false;
    }
    if (s.header.id != id) {
        LOG_WARN("StyleLibrary::update_style: header.id mismatch ('{}' vs '{}')",
                 s.header.id, id);
        return false;
    }
    auto it = find_user(id);
    if (it == m_user_styles.end()) {
        LOG_WARN("StyleLibrary::update_style: id '{}' not found in user list", id);
        return false;
    }
    *it = std::move(s);
    m_sig_changed.emit(id);
    return true;
}

bool StyleLibrary::remove_style(const StyleId& id) {
    if (style::is_built_in(id)) {
        LOG_WARN("StyleLibrary::remove_style: '{}' is built-in (read-only)", id);
        return false;
    }
    auto it = find_user(id);
    if (it == m_user_styles.end()) {
        LOG_WARN("StyleLibrary::remove_style: id '{}' not found in user list", id);
        return false;
    }
    m_user_styles.erase(it);
    m_sig_removed.emit(id);
    return true;
}

StyleId StyleLibrary::duplicate_to_user(const StyleId& src) {
    const Style* p = find_style(src);
    if (!p) {
        LOG_WARN("StyleLibrary::duplicate_to_user: id '{}' not found", src);
        return {};
    }
    Style copy = *p;                        // by value — safe across vector growth
    copy.header.id   = generate_unique_user_id();
    if (copy.header.id.empty()) return {};
    // Disambiguate the display name. Phase 2 panel UX may want " copy 2",
    // " copy 3" etc. for repeated dupes; m1 keeps it simple — the user
    // can rename right after.
    if (!copy.header.name.empty()) copy.header.name += " copy";
    StyleId id = copy.header.id;
    m_user_styles.push_back(std::move(copy));
    m_sig_added.emit(id);
    return id;
}

// ── Lookup ──────────────────────────────────────────────────────────────────

const Style* StyleLibrary::find_style(const StyleId& id) const {
    // User first, then app — matches SwatchLibrary's custom-first reads
    // even though no collision is possible by id-prefix construction.
    if (auto it = find_user(id); it != m_user_styles.end()) return &*it;
    if (auto it = find_app(id);  it != m_app_styles.end())  return &*it;
    return nullptr;
}

bool StyleLibrary::is_built_in(const StyleId& id) const {
    // Tighter than the free predicate — actually checks the app list. A
    // typo'd "app:nonexistent" returns false here even though the free
    // is_built_in() returns true on the prefix alone. Used by UI gating
    // where a "built-in but unknown" id should fall through to "treat as
    // unbound" rather than "treat as read-only".
    return find_app(id) != m_app_styles.end();
}

// ── Category accessors ──────────────────────────────────────────────────────

std::vector<const Style*>
StyleLibrary::app_styles_in_category(const std::string& cat) const {
    std::vector<const Style*> out;
    for (const Style& s : m_app_styles) {
        if (s.header.category == cat) out.push_back(&s);
    }
    return out;
}

std::vector<const Style*>
StyleLibrary::user_styles_in_category(const std::string& cat) const {
    std::vector<const Style*> out;
    for (const Style& s : m_user_styles) {
        if (s.header.category == cat) out.push_back(&s);
    }
    return out;
}

std::vector<std::string> StyleLibrary::user_categories() const {
    // First-seen insertion order. unordered_set tracks dedup; output is
    // a vector so callers iterate in display order.
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const Style& s : m_user_styles) {
        if (seen.insert(s.header.category).second) {
            out.push_back(s.header.category);
        }
    }
    return out;
}

std::vector<std::string> StyleLibrary::app_categories() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const Style& s : m_app_styles) {
        if (seen.insert(s.header.category).second) {
            out.push_back(s.header.category);
        }
    }
    return out;
}

// ── JSON round-trip (S76 m3b) ───────────────────────────────────────────────
//
// User-tier only. Per-entry shape:
//   {
//     "header": {"id": "stl_...", "name": "...", "category": "..."},
//     "fill":   <paint-json>,
//     "stroke": {
//       "paint":       <paint-json>,
//       "width":       <number>,
//       "cap":         "butt" | "round" | "square",
//       "join":        "miter" | "round" | "bevel",
//       "miter_limit": <number>
//     }
//   }
//
// S87 m1: dash, dash_offset, and align were removed. They had been
// specced for a future phase but never reached the renderer; clean
// removal lets the model honestly reflect what the app supports.
// JSON loads of older files with these keys are tolerant — the load
// path uses .value() with defaults for known keys and ignores unknown
// keys, so old files round-trip with the deprecated fields silently
// dropped.
//
// File-scope helpers sit in the anonymous namespace alongside the style
// translation unit's other internal shapes. Match SwatchLibrary's
// write_pool / load_pool naming convention for symmetry.

namespace {

using json = nlohmann::json;

// LineCap / LineJoin <-> string. Tiny tables beat std::map here — two
// passes, no allocations, trivially greppable.

const char* cap_to_str(LineCap c) {
    switch (c) {
        case LineCap::Butt:   return "butt";
        case LineCap::Round:  return "round";
        case LineCap::Square: return "square";
    }
    return "butt";
}

LineCap cap_from_str(const std::string& s) {
    if (s == "butt")   return LineCap::Butt;
    if (s == "round")  return LineCap::Round;
    if (s == "square") return LineCap::Square;
    LOG_WARN("StyleLibrary JSON: unknown cap '{}', using butt", s);
    return LineCap::Butt;
}

const char* join_to_str(LineJoin j) {
    switch (j) {
        case LineJoin::Miter: return "miter";
        case LineJoin::Round: return "round";
        case LineJoin::Bevel: return "bevel";
    }
    return "miter";
}

LineJoin join_from_str(const std::string& s) {
    if (s == "miter") return LineJoin::Miter;
    if (s == "round") return LineJoin::Round;
    if (s == "bevel") return LineJoin::Bevel;
    LOG_WARN("StyleLibrary JSON: unknown join '{}', using miter", s);
    return LineJoin::Miter;
}

} // anon namespace — helpers below at namespace style:: so the
//                   StyleIO bridge (S102 m1) can call style_to_json /
//                   style_from_json directly with the same shape used
//                   by to_user_json / from_user_json.

json style_to_json(const Style& s) {
    json entry;
    entry["header"] = {
        {"id",       s.header.id},
        {"name",     s.header.name},
        {"category", s.header.category}
    };
    entry["fill"]   = color::paint_to_json(s.fill);
    entry["stroke"] = {
        {"paint",       color::paint_to_json(s.stroke.paint)},
        {"width",       s.stroke.width},
        {"cap",         cap_to_str(s.stroke.cap)},
        {"join",        join_to_str(s.stroke.join)},
        {"miter_limit", s.stroke.miter_limit}
    };
    // S98: shadow as a third sibling block. Same shape as the
    // data-curvz-shadow-* SVG attribute set, persisted into project.json.
    // Older library versions reading this file will ignore the unknown
    // key (nlohmann::json silently drops keys nobody asks about) — the
    // style still loads with default ShadowAppearance, so older builds
    // see "no shadow" rather than a parse failure.
    entry["shadow"] = {
        {"enabled", s.shadow.enabled},
        {"dx",      s.shadow.dx},
        {"dy",      s.shadow.dy},
        {"blur",    s.shadow.blur},
        {"color_r", s.shadow.color_r},
        {"color_g", s.shadow.color_g},
        {"color_b", s.shadow.color_b},
        {"color_a", s.shadow.color_a},
        {"opacity", s.shadow.opacity}
    };
    return entry;
}

// Returns std::nullopt if the entry is unusable (empty id, built-in id,
// malformed structure). The outer loader skips those rather than
// short-circuiting — one bad entry shouldn't destroy the rest of the
// user tier.
std::optional<Style> style_from_json(const json& entry) {
    if (!entry.is_object()) {
        LOG_WARN("StyleLibrary JSON: style entry not an object, skipping");
        return std::nullopt;
    }

    Style s;

    // Header. Nested object; tolerate shallow-missing structure.
    if (entry.contains("header") && entry["header"].is_object()) {
        const auto& h = entry["header"];
        s.header.id       = h.value("id",       std::string{});
        s.header.name     = h.value("name",     std::string{});
        s.header.category = h.value("category", std::string{});
    }
    if (s.header.id.empty()) {
        LOG_WARN("StyleLibrary JSON: style entry missing header.id, skipping");
        return std::nullopt;
    }
    if (style::is_built_in(s.header.id)) {
        // Defensive: 'app:' ids belong exclusively to the hardcoded
        // constructor list. Any 'app:' id in the user tier — whether
        // from a tampered-with project.json or a bug elsewhere — is
        // dropped rather than loaded.
        LOG_WARN("StyleLibrary JSON: refusing 'app:' id '{}' in user tier, "
                 "skipping", s.header.id);
        return std::nullopt;
    }

    // Fill. Missing key → None (paint_from_json's default). This lets
    // older projects without fills round-trip as "no fill".
    if (entry.contains("fill")) {
        s.fill = color::paint_from_json(entry["fill"]);
    } else {
        s.fill = color::None{};
    }

    // Stroke. All sub-fields default to StrokeAppearance's defaults
    // when missing.
    if (entry.contains("stroke") && entry["stroke"].is_object()) {
        const auto& st = entry["stroke"];
        if (st.contains("paint")) {
            s.stroke.paint = color::paint_from_json(st["paint"]);
        }
        s.stroke.width       = st.value("width",       s.stroke.width);
        s.stroke.cap         = cap_from_str(st.value("cap",  std::string{"butt"}));
        s.stroke.join        = join_from_str(st.value("join", std::string{"miter"}));
        s.stroke.miter_limit = st.value("miter_limit", s.stroke.miter_limit);
        // S87 m1: legacy "dash", "dash_offset", "align" keys in older
        // project.json files are silently ignored — nlohmann::json will
        // simply not invoke any reader for keys we don't ask about.
    }

    // Shadow (S98). Every sub-field defaults to ShadowAppearance's
    // defaults when missing — and the entire "shadow" key is optional.
    // Older project.json files written before S98 simply lack the key
    // and load with shadow disabled (matches the new-style default).
    if (entry.contains("shadow") && entry["shadow"].is_object()) {
        const auto& sh = entry["shadow"];
        s.shadow.enabled = sh.value("enabled", s.shadow.enabled);
        s.shadow.dx      = sh.value("dx",      s.shadow.dx);
        s.shadow.dy      = sh.value("dy",      s.shadow.dy);
        s.shadow.blur    = sh.value("blur",    s.shadow.blur);
        s.shadow.color_r = sh.value("color_r", s.shadow.color_r);
        s.shadow.color_g = sh.value("color_g", s.shadow.color_g);
        s.shadow.color_b = sh.value("color_b", s.shadow.color_b);
        s.shadow.color_a = sh.value("color_a", s.shadow.color_a);
        s.shadow.opacity = sh.value("opacity", s.shadow.opacity);
    }

    return s;
}

void StyleLibrary::to_user_json(json& j) const {
    json arr = json::array();
    for (const Style& s : m_user_styles) {
        arr.push_back(style_to_json(s));
    }
    j["styles"] = std::move(arr);
}

void StyleLibrary::from_user_json(const json& j) {
    // Atomic replace: wipe first, then populate. Listeners don't see a
    // mid-load intermediate state because no signals fire during the
    // load (see header note).
    m_user_styles.clear();

    if (!j.contains("styles") || !j["styles"].is_array()) {
        LOG_INFO("StyleLibrary::from_user_json: no 'styles' array, user tier "
                 "left empty");
        return;
    }

    std::size_t skipped = 0;
    for (const auto& entry : j["styles"]) {
        auto s = style_from_json(entry);
        if (!s) { ++skipped; continue; }
        // Collision check: user-tier id already inserted in this same
        // load, OR collides with an app id. Either is a project-file
        // corruption case; skip with a warning.
        if (find_user(s->header.id) != m_user_styles.end() ||
            find_app(s->header.id)  != m_app_styles.end()) {
            LOG_WARN("StyleLibrary::from_user_json: id '{}' collides, skipping",
                     s->header.id);
            ++skipped;
            continue;
        }
        m_user_styles.push_back(std::move(*s));
    }

    if (skipped > 0) {
        LOG_INFO("StyleLibrary::from_user_json: loaded {} user style(s), "
                 "skipped {}", m_user_styles.size(), skipped);
    } else {
        LOG_INFO("StyleLibrary::from_user_json: loaded {} user style(s)",
                 m_user_styles.size());
    }
}

} // namespace style
} // namespace Curvz
