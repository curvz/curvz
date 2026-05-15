//
// SwatchLibrary.cpp — two-tier registry (Phase 5 / s69).
//
// Internally two pools per kind: defaults (shipped, read-only at the
// library's behaviour level) and custom (per-project, mutable). Reads
// check custom first. Writes target custom. Cross-tier palette→swatch
// references are first-class.
//
// File I/O is split: to_custom_json / from_custom_json round-trip the
// per-project file; to_defaults_json / from_defaults_json round-trip
// the app-global defaults file. seed_defaults_from copies the loaded
// defaults pool from Application's master library into each new
// project's library (option B — data duplication over shared pointer
// plumbing).
//
// Per-project working state (recents, active_palette) is NOT part of
// either tier's JSON — it persists via project.json's editor_state
// block (CurvzProject handles that glue).
//

#include "color/SwatchLibrary.hpp"
#include "color/FillStyleInterop.hpp"
#include "style/StyleInterop.hpp"  // mutate_appearance — set_paint routes user-driven
                                   // appearance writes through the Style funnel (S74 m2)
#include "CurvzLog.hpp"
#include "SceneNode.hpp"  // generate_internal_id() — GLib UUID v4;
                          // FillStyle / StrokeStyle definitions used by
                          // set_paint to read and write the object's slots.

#include <nlohmann/json.hpp>
#include <glibmm/miscutils.h>  // Glib::get_user_config_dir() for defaults path

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Curvz {
namespace color {

using json = nlohmann::json;

// ── id generation ───────────────────────────────────────────────────────────
//
// Same UUID generator as SceneNode (g_uuid_string_random via
// generate_internal_id()). With two pools we check both to avoid
// issuing an id that collides with a default — important because a
// user-created swatch must never shadow a default.

template <typename MapT>
std::string SwatchLibrary::generate_unique_id(const MapT& existing_custom,
                                              const MapT& existing_defaults) const {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string id = generate_internal_id();
        if (existing_custom.find(id)   == existing_custom.end() &&
            existing_defaults.find(id) == existing_defaults.end()) {
            return id;
        }
    }
    LOG_ERROR("SwatchLibrary::generate_unique_id: 2 consecutive UUID "
              "collisions (custom {}, defaults {}) — RNG failure?",
              existing_custom.size(), existing_defaults.size());
    return {};
}

// ── Swatch CRUD ─────────────────────────────────────────────────────────────

SwatchId SwatchLibrary::add_swatch(Swatch s) {
    SwatchHeader& h = swatch_header(s);
    if (h.id.empty()) {
        h.id = generate_unique_id(m_custom_swatches, m_defaults_swatches);
        if (h.id.empty()) return {};
    } else {
        // Either tier owning this id blocks the insert.
        if (m_custom_swatches.find(h.id) != m_custom_swatches.end() ||
            m_defaults_swatches.find(h.id) != m_defaults_swatches.end()) {
            LOG_WARN("SwatchLibrary::add_swatch: id '{}' already exists, refusing",
                     h.id);
            return {};
        }
    }
    SwatchId id = h.id;
    m_custom_swatches.emplace(id, std::move(s));
    // s220 m1a hotfix: fire the add signal so listeners (panel) refresh
    // regardless of who triggered the add — direct panel call, undo/redo,
    // or future scripted CRUD. The library is the only source of truth
    // for "your view is stale"; the panel can no longer rely on always
    // being the originator.
    m_sig_swatch_added.emit(id);
    return id;
}

bool SwatchLibrary::update_swatch(const SwatchId& id, Swatch s) {
    // Read-only guard: ids in defaults cannot be mutated.
    if (m_defaults_swatches.find(id) != m_defaults_swatches.end()) {
        LOG_WARN("SwatchLibrary::update_swatch: id '{}' is a default "
                 "(read-only) — use duplicate-to-edit", id);
        return false;
    }
    auto it = m_custom_swatches.find(id);
    if (it == m_custom_swatches.end()) {
        LOG_WARN("SwatchLibrary::update_swatch: id '{}' not found", id);
        return false;
    }
    if (swatch_header(s).id != id) {
        LOG_WARN("SwatchLibrary::update_swatch: id mismatch (arg '{}', "
                 "payload '{}')", id, swatch_header(s).id);
        return false;
    }
    it->second = std::move(s);
    // Fire the live-recolour signal. Canvas (the only current listener)
    // walks the doc tree, refreshes the cached FillStyle on any SceneNode
    // whose fill_swatch_id / stroke_swatch_id matches, and queues a draw.
    m_sig_swatch_changed.emit(id);
    return true;
}

bool SwatchLibrary::remove_swatch(const SwatchId& id) {
    // Read-only guard.
    if (m_defaults_swatches.find(id) != m_defaults_swatches.end()) {
        LOG_WARN("SwatchLibrary::remove_swatch: id '{}' is a default "
                 "(read-only)", id);
        return false;
    }
    auto it = m_custom_swatches.find(id);
    if (it == m_custom_swatches.end()) return false;

    m_custom_swatches.erase(it);

    // Strip references from CUSTOM palettes only. Defaults palettes may
    // reference this id but, by construction, only if the id lived in
    // defaults — and we just rejected that case above. So defaults
    // palettes don't need scrubbing here.
    for (auto& [pid, pal] : m_custom_palettes) {
        auto& sw = pal.swatches;
        sw.erase(std::remove(sw.begin(), sw.end(), id), sw.end());
    }

    // Recents are per-project / mutable; always scrub.
    m_recents.erase(std::remove(m_recents.begin(), m_recents.end(), id),
                    m_recents.end());

    // Objects whose fill_swatch_id / stroke_swatch_id (upcoming
    // milestone) referenced this swatch will have dangling ids after
    // this call. They continue to render correctly via their cached
    // FillStyle colour; the delete-with-usage-check flow is the later
    // milestone that gives the user an informed choice first.

    // s220 m1a hotfix: fire the remove signal so listeners (panel)
    // refresh regardless of originator. See add_swatch's matching
    // comment.
    m_sig_swatch_removed.emit(id);
    return true;
}

bool SwatchLibrary::rename_swatch(const SwatchId& id,
                                  const std::string& new_name) {
    if (m_defaults_swatches.find(id) != m_defaults_swatches.end()) {
        LOG_WARN("SwatchLibrary::rename_swatch: id '{}' is a default "
                 "(read-only)", id);
        return false;
    }
    auto it = m_custom_swatches.find(id);
    if (it == m_custom_swatches.end()) return false;
    swatch_header(it->second).name = new_name;
    return true;
}

const Swatch* SwatchLibrary::find_swatch(const SwatchId& id) const {
    // Custom first — user overrides never happen today (add_swatch
    // blocks id collisions), but the precedence rule is still the
    // right one because any id we find in custom IS a custom swatch.
    auto it = m_custom_swatches.find(id);
    if (it != m_custom_swatches.end()) return &it->second;
    auto dit = m_defaults_swatches.find(id);
    if (dit != m_defaults_swatches.end()) return &dit->second;
    return nullptr;
}

bool SwatchLibrary::is_default_swatch(const SwatchId& id) const {
    return m_defaults_swatches.find(id) != m_defaults_swatches.end();
}

std::vector<SwatchId> SwatchLibrary::all_swatch_ids() const {
    std::vector<SwatchId> out;
    out.reserve(m_defaults_swatches.size() + m_custom_swatches.size());
    // Defaults first — UI convention: shipped work above user work.
    for (const auto& [id, _] : m_defaults_swatches) out.push_back(id);
    for (const auto& [id, _] : m_custom_swatches)   out.push_back(id);
    return out;
}

// ── Palette CRUD ────────────────────────────────────────────────────────────

PaletteId SwatchLibrary::add_palette(Palette p) {
    if (p.id.empty()) {
        p.id = generate_unique_id(m_custom_palettes, m_defaults_palettes);
        if (p.id.empty()) return {};
    } else {
        if (m_custom_palettes.find(p.id) != m_custom_palettes.end() ||
            m_defaults_palettes.find(p.id) != m_defaults_palettes.end()) {
            LOG_WARN("SwatchLibrary::add_palette: id '{}' already exists, refusing",
                     p.id);
            return {};
        }
    }
    PaletteId id = p.id;
    m_custom_palettes.emplace(id, std::move(p));
    return id;
}

bool SwatchLibrary::remove_palette(const PaletteId& id) {
    if (m_defaults_palettes.find(id) != m_defaults_palettes.end()) {
        LOG_WARN("SwatchLibrary::remove_palette: id '{}' is a default "
                 "(read-only) — duplicate to edit first", id);
        return false;
    }
    auto it = m_custom_palettes.find(id);
    if (it == m_custom_palettes.end()) return false;
    m_custom_palettes.erase(it);
    if (m_active_palette == id) m_active_palette.clear();
    return true;
}

bool SwatchLibrary::rename_palette(const PaletteId& id,
                                   const std::string& new_name) {
    if (m_defaults_palettes.find(id) != m_defaults_palettes.end()) {
        LOG_WARN("SwatchLibrary::rename_palette: id '{}' is a default "
                 "(read-only)", id);
        return false;
    }
    auto it = m_custom_palettes.find(id);
    if (it == m_custom_palettes.end()) return false;
    it->second.name = new_name;
    return true;
}

bool SwatchLibrary::add_to_palette(const PaletteId& pid, const SwatchId& sid) {
    if (m_defaults_palettes.find(pid) != m_defaults_palettes.end()) {
        LOG_WARN("SwatchLibrary::add_to_palette: palette '{}' is a default "
                 "(read-only) — duplicate to edit first", pid);
        return false;
    }
    auto pit = m_custom_palettes.find(pid);
    if (pit == m_custom_palettes.end()) {
        LOG_WARN("SwatchLibrary::add_to_palette: palette '{}' not found", pid);
        return false;
    }
    if (pit->second.builtin) {
        // The builtin flag predates the two-tier split. A custom-tier
        // palette with builtin=true is a defensive state — duplicate
        // to edit still applies.
        LOG_WARN("SwatchLibrary::add_to_palette: palette '{}' has "
                 "builtin=true — duplicate to edit", pid);
        return false;
    }
    // Cross-tier swatch refs are first-class — check both pools.
    if (m_custom_swatches.find(sid) == m_custom_swatches.end() &&
        m_defaults_swatches.find(sid) == m_defaults_swatches.end()) {
        LOG_WARN("SwatchLibrary::add_to_palette: swatch '{}' not found", sid);
        return false;
    }
    auto& sw = pit->second.swatches;
    if (std::find(sw.begin(), sw.end(), sid) != sw.end()) {
        return true;  // idempotent
    }
    sw.push_back(sid);
    return true;
}

bool SwatchLibrary::remove_from_palette(const PaletteId& pid,
                                        const SwatchId& sid) {
    if (m_defaults_palettes.find(pid) != m_defaults_palettes.end()) {
        LOG_WARN("SwatchLibrary::remove_from_palette: palette '{}' is a "
                 "default (read-only)", pid);
        return false;
    }
    auto pit = m_custom_palettes.find(pid);
    if (pit == m_custom_palettes.end()) return false;
    auto& sw = pit->second.swatches;
    auto before = sw.size();
    sw.erase(std::remove(sw.begin(), sw.end(), sid), sw.end());
    return sw.size() != before;
}

bool SwatchLibrary::reorder_in_palette(const PaletteId& pid,
                                       const SwatchId& sid,
                                       std::size_t new_index) {
    if (m_defaults_palettes.find(pid) != m_defaults_palettes.end()) {
        LOG_WARN("SwatchLibrary::reorder_in_palette: palette '{}' is a "
                 "default (read-only)", pid);
        return false;
    }
    auto pit = m_custom_palettes.find(pid);
    if (pit == m_custom_palettes.end()) return false;
    auto& sw = pit->second.swatches;
    auto it = std::find(sw.begin(), sw.end(), sid);
    if (it == sw.end()) return false;

    SwatchId id = *it;
    sw.erase(it);
    if (new_index > sw.size()) new_index = sw.size();
    sw.insert(sw.begin() + static_cast<std::ptrdiff_t>(new_index), id);
    return true;
}

PaletteId SwatchLibrary::duplicate_palette(const PaletteId& pid,
                                           const std::string& new_name) {
    // Source may live in EITHER tier. This is the blessed "edit a
    // default" path — copy it into custom, then the user edits the copy.
    const Palette* source = nullptr;
    auto cit = m_custom_palettes.find(pid);
    if (cit != m_custom_palettes.end()) source = &cit->second;
    else {
        auto dit = m_defaults_palettes.find(pid);
        if (dit != m_defaults_palettes.end()) source = &dit->second;
    }
    if (!source) return {};

    Palette copy = *source;
    copy.id.clear();               // force new id generation
    copy.name = new_name.empty() ? (source->name + " copy") : new_name;
    copy.builtin = false;          // duplicates are always user-editable
    copy.source = "user";
    // Swatch list is copied by value; cross-tier references survive because
    // find_swatch checks both pools.
    return add_palette(std::move(copy));
}

const Palette* SwatchLibrary::find_palette(const PaletteId& id) const {
    auto it = m_custom_palettes.find(id);
    if (it != m_custom_palettes.end()) return &it->second;
    auto dit = m_defaults_palettes.find(id);
    if (dit != m_defaults_palettes.end()) return &dit->second;
    return nullptr;
}

bool SwatchLibrary::is_default_palette(const PaletteId& id) const {
    return m_defaults_palettes.find(id) != m_defaults_palettes.end();
}

std::vector<PaletteId> SwatchLibrary::all_palette_ids() const {
    std::vector<PaletteId> out;
    out.reserve(m_defaults_palettes.size() + m_custom_palettes.size());
    for (const auto& [id, _] : m_defaults_palettes) out.push_back(id);
    for (const auto& [id, _] : m_custom_palettes)   out.push_back(id);
    return out;
}

// ── Active palette ──────────────────────────────────────────────────────────

void SwatchLibrary::set_active_palette(const PaletteId& id) {
    if (id.empty()) {
        m_active_palette.clear();
        return;
    }
    // Active may reference either tier.
    if (m_custom_palettes.find(id)   == m_custom_palettes.end() &&
        m_defaults_palettes.find(id) == m_defaults_palettes.end()) {
        LOG_WARN("SwatchLibrary::set_active_palette: '{}' not found", id);
        return;
    }
    m_active_palette = id;
}

// ── Recents ─────────────────────────────────────────────────────────────────

void SwatchLibrary::touch_recent(const SwatchId& id) {
    // Accept ids from either tier.
    if (m_custom_swatches.find(id)   == m_custom_swatches.end() &&
        m_defaults_swatches.find(id) == m_defaults_swatches.end()) {
        return;
    }
    m_recents.erase(std::remove(m_recents.begin(), m_recents.end(), id),
                    m_recents.end());
    m_recents.insert(m_recents.begin(), id);
    if (m_recents.size() > RECENTS_CAPACITY) {
        m_recents.resize(RECENTS_CAPACITY);
    }
}

// ── Convenience for UI ──────────────────────────────────────────────────────

SwatchId SwatchLibrary::find_solid_by_color(const Color& c) const {
    // Custom first — a user who named their own colour wins over a
    // shipped default with the same hex.
    for (const auto& [id, sw] : m_custom_swatches) {
        if (const auto* solid = std::get_if<SolidSwatch>(&sw)) {
            if (solid->color == c) return id;
        }
    }
    for (const auto& [id, sw] : m_defaults_swatches) {
        if (const auto* solid = std::get_if<SolidSwatch>(&sw)) {
            if (solid->color == c) return id;
        }
    }
    return {};
}

// ── Paint binding (Phase 5 M2 + s70 SceneNode bindings) ────────────────────
//
// The SceneNode *_swatch_id fields are the source of truth for "is this paint
// a swatch reference?" — the FillStyle / StrokeStyle::paint caches are the
// resolved render-path colour. Non-empty id ⇒ reconstruct a SwatchRef with
// the cached FillStyle colour as the fallback. Empty id ⇒ round-trip through
// the FillStyle enum (None / CurrentColor / Solid) exactly as before.
//
// See HANDOFF s69 "paint is a reference, not a colour": when non-solid
// swatch kinds land (gradient, pattern), the renderer will dispatch on the
// swatch via id rather than reading FillStyle. The id field carries that
// forward; the FillStyle cache is just a fast path for today's solids.

Paint SwatchLibrary::get_paint(const SceneNode& obj, PaintSlot slot) const {
    const FillStyle& fs = (slot == PaintSlot::Fill) ? obj.fill
                                                    : obj.stroke.paint;
    const std::string& sid = (slot == PaintSlot::Fill) ? obj.fill_swatch_id
                                                       : obj.stroke_swatch_id;

    // Bound to a swatch: reconstruct a SwatchRef. Seed its fallback from the
    // currently cached FillStyle colour so a dead-ref path in resolve_paint
    // degrades to what the user last saw rendered, not to uninitialised bytes.
    // Equality ignores the fallback (see Paint.hpp operator==), so a cache
    // that drifted from the live swatch between edits doesn't break identity.
    if (!sid.empty()) {
        return SwatchRef{ sid, Color{ fs.r, fs.g, fs.b, fs.a } };
    }

    return to_paint(fs);
}

bool SwatchLibrary::set_paint(SceneNode& obj, PaintSlot slot, const Paint& p) {
    // Extract the binding id from the incoming Paint BEFORE we flatten to
    // FillStyle — to_fillstyle loses the SwatchRef distinction, which is
    // the whole bug this milestone fixes. Every other variant clears the id.
    std::string new_id;
    if (const auto* sr = std::get_if<SwatchRef>(&p)) {
        new_id = sr->id;
    }

    // Validate slot BEFORE entering the funnel. The funnel commits to the
    // appearance change (breaks bound_style) the moment fn runs; a late
    // bad-slot bail-out would leave a node half-mutated. An unknown slot
    // is a caller bug regardless, so reject early.
    if (slot != PaintSlot::Fill && slot != PaintSlot::Stroke) {
        LOG_WARN("SwatchLibrary::set_paint: unknown slot {}",
                 static_cast<int>(slot));
        return false;
    }

    // to_fillstyle with the library-aware overload resolves any SwatchRef
    // to a concrete Solid via find_swatch (which searches both tiers). The
    // resolved FillStyle is the render-path cache on the node; the id field
    // is the actual binding.
    FillStyle new_fs = to_fillstyle(p, *this);

    // Route through the Style-system funnel. Swatch apply is a user-driven
    // appearance write, so it must break any existing Style binding on the
    // object per the addendum's override-unbinds rule. See S74 m2
    // migration notes in include/style/StyleInterop.hpp.
    style::mutate_appearance(obj, [&](SceneNode& n) {
        if (slot == PaintSlot::Fill) {
            n.fill = new_fs;
            n.fill_swatch_id = new_id;
        } else {  // Stroke — validated above
            n.stroke.paint = new_fs;
            n.stroke_swatch_id = new_id;
        }
    });

    LOG_DEBUG("SwatchLibrary::set_paint: iid='{}' slot={} swatch='{}' "
              "fs=#{:02x}{:02x}{:02x}{:02x}",
              obj.internal_id, static_cast<int>(slot),
              new_id,
              channel_to_u8(new_fs.r), channel_to_u8(new_fs.g),
              channel_to_u8(new_fs.b), channel_to_u8(new_fs.a));
    m_sig_paint_changed.emit(&obj, slot);
    return true;
}

// ── JSON round-trip ─────────────────────────────────────────────────────────
//
// Shared serialise helpers — used by both tier round-trips. Each tier's
// file is just { "swatches": [...], "palettes": [...] }. No
// schema_version; see HANDOFF "two-tier config principle".

namespace {

void write_pool(json& j,
                const std::map<SwatchId, Swatch>& swatches,
                const std::map<PaletteId, Palette>& palettes) {
    json swatch_arr = json::array();
    for (const auto& [id, sw] : swatches) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            json entry;
            entry["id"]    = v.header.id;
            entry["name"]  = v.header.name;
            entry["notes"] = v.header.notes;
            entry["tags"]  = v.header.tags;
            if constexpr (std::is_same_v<T, SolidSwatch>) {
                entry["kind"]  = "solid";
                entry["color"] = to_hex(v.color);
            } else {
                LOG_ERROR("SwatchLibrary::write_pool: unhandled swatch kind");
            }
            swatch_arr.push_back(std::move(entry));
        }, sw);
    }
    j["swatches"] = std::move(swatch_arr);

    json palette_arr = json::array();
    for (const auto& [id, p] : palettes) {
        json entry;
        entry["id"]          = p.id;
        entry["name"]        = p.name;
        entry["description"] = p.description;
        entry["source"]      = p.source;
        entry["builtin"]     = p.builtin;
        entry["swatches"]    = p.swatches;
        palette_arr.push_back(std::move(entry));
    }
    j["palettes"] = std::move(palette_arr);
}

// Loads swatches + palettes from a json blob into the target maps.
// Used by both tier loaders and by the legacy migration.
//
// `check_other_swatches` is the OTHER-tier swatch map; palette entries
// whose swatch references don't resolve in either this tier or the
// other are dropped with a warning. For the legacy migration, pass an
// empty map — there's only one tier at migration time.
void load_pool(const json& j,
               std::map<SwatchId, Swatch>& swatches,
               std::map<PaletteId, Palette>& palettes,
               const std::map<SwatchId, Swatch>& check_other_swatches) {
    swatches.clear();
    palettes.clear();

    if (j.contains("swatches") && j["swatches"].is_array()) {
        for (const auto& entry : j["swatches"]) {
            std::string kind = entry.value("kind", "solid");
            SwatchHeader h;
            h.id    = entry.value("id", std::string{});
            h.name  = entry.value("name", std::string{});
            h.notes = entry.value("notes", std::string{});
            if (entry.contains("tags") && entry["tags"].is_array()) {
                for (const auto& t : entry["tags"]) {
                    if (t.is_string()) h.tags.push_back(t.get<std::string>());
                }
            }
            if (h.id.empty()) {
                LOG_WARN("SwatchLibrary::load_pool: swatch entry missing id, "
                         "skipping");
                continue;
            }

            if (kind == "solid") {
                SolidSwatch s;
                s.header = std::move(h);
                std::string hex = entry.value("color", std::string{"#000000"});
                auto parsed = from_hex(hex);
                if (!parsed) {
                    LOG_WARN("SwatchLibrary::load_pool: swatch '{}' color "
                             "'{}' unparseable, using black", s.header.id, hex);
                    s.color = Color::black();
                } else {
                    s.color = *parsed;
                }
                // Extract the id before the emplace — see s68 bug note
                // about order-of-evaluation with std::move'd values.
                SwatchId key = s.header.id;
                swatches.emplace(std::move(key), Swatch{std::move(s)});
            } else {
                LOG_WARN("SwatchLibrary::load_pool: swatch '{}' has unknown "
                         "kind '{}', skipping", h.id, kind);
            }
        }
    }

    if (j.contains("palettes") && j["palettes"].is_array()) {
        for (const auto& entry : j["palettes"]) {
            Palette p;
            p.id          = entry.value("id", std::string{});
            p.name        = entry.value("name", std::string{});
            p.description = entry.value("description", std::string{});
            p.source      = entry.value("source", std::string{});
            p.builtin     = entry.value("builtin", false);

            if (entry.contains("swatches") && entry["swatches"].is_array()) {
                for (const auto& sid : entry["swatches"]) {
                    if (!sid.is_string()) continue;
                    std::string id = sid.get<std::string>();
                    // Keep the reference if it resolves in THIS tier or
                    // the OTHER tier. Drop only truly dangling ids.
                    bool resolves = swatches.find(id) != swatches.end()
                                 || check_other_swatches.find(id)
                                        != check_other_swatches.end();
                    if (resolves) {
                        p.swatches.push_back(id);
                    } else {
                        LOG_WARN("SwatchLibrary::load_pool: palette '{}' "
                                 "references missing swatch '{}', skipping ref",
                                 p.id, id);
                    }
                }
            }

            if (p.id.empty()) {
                LOG_WARN("SwatchLibrary::load_pool: palette entry missing id, "
                         "skipping");
                continue;
            }
            PaletteId key = p.id;
            palettes.emplace(std::move(key), std::move(p));
        }
    }
}

} // anon namespace

void SwatchLibrary::to_custom_json(json& j) const {
    write_pool(j, m_custom_swatches, m_custom_palettes);
}

void SwatchLibrary::from_custom_json(const json& j) {
    // Custom palette swatch-refs may resolve via defaults — pass the
    // defaults swatches pool as the "other tier" check.
    load_pool(j, m_custom_swatches, m_custom_palettes, m_defaults_swatches);
    LOG_INFO("SwatchLibrary::from_custom_json: loaded {} custom swatch(es), "
             "{} custom palette(s)",
             m_custom_swatches.size(), m_custom_palettes.size());
}

void SwatchLibrary::to_defaults_json(json& j) const {
    write_pool(j, m_defaults_swatches, m_defaults_palettes);
}

void SwatchLibrary::from_defaults_json(const json& j) {
    // Defaults palette swatch-refs should resolve within defaults —
    // but tolerate custom-tier refs too for symmetry. At startup the
    // custom pool is empty anyway.
    load_pool(j, m_defaults_swatches, m_defaults_palettes, m_custom_swatches);
    LOG_INFO("SwatchLibrary::from_defaults_json: loaded {} default swatch(es), "
             "{} default palette(s)",
             m_defaults_swatches.size(), m_defaults_palettes.size());
}

void SwatchLibrary::seed_defaults_from(const SwatchLibrary& source) {
    // Simple map copy. Defaults are a small pool (~50 entries expected);
    // per-project duplication is cheap and removes any shared-pointer
    // lifetime questions.
    m_defaults_swatches = source.m_defaults_swatches;
    m_defaults_palettes = source.m_defaults_palettes;
}

// ── Legacy project.json migration ──────────────────────────────────────────
//
// Pre-s69 projects kept everything (swatches, palettes, recents,
// active_palette) in project.json's `color_system` block. After the
// two-tier split:
//   * swatches + palettes move to <project>.curvz/swatches.json (custom tier)
//   * recents + active_palette stay in project.json (editor_state)
//
// CurvzProject::load calls this method on the `color_system` block when
// the sibling swatches.json doesn't exist yet. On next save the project
// writes the new layout, and the `color_system` block disappears.

void SwatchLibrary::load_legacy_color_system(const json& j) {
    // Custom tier swatches + palettes. Pass empty "other" map because
    // at migration time the defaults tier is either empty or has just
    // been seeded — either way, references in the legacy block are
    // to ids that lived inside that same block, so load_pool's own-tier
    // check is sufficient.
    load_pool(j, m_custom_swatches, m_custom_palettes,
              /* check_other_swatches */ m_defaults_swatches);

    // Recents + active_palette — lift them to per-project state. These
    // used to live inside color_system's own schema; now CurvzProject
    // will write them to editor_state going forward.
    m_recents.clear();
    if (j.contains("recents") && j["recents"].is_array()) {
        for (const auto& sid : j["recents"]) {
            if (!sid.is_string()) continue;
            std::string id = sid.get<std::string>();
            if (m_custom_swatches.find(id)   != m_custom_swatches.end() ||
                m_defaults_swatches.find(id) != m_defaults_swatches.end()) {
                m_recents.push_back(id);
            }
        }
        if (m_recents.size() > RECENTS_CAPACITY) {
            m_recents.resize(RECENTS_CAPACITY);
        }
    }

    m_active_palette.clear();
    std::string active = j.value("active_palette", std::string{});
    if (!active.empty() &&
        (m_custom_palettes.find(active)   != m_custom_palettes.end() ||
         m_defaults_palettes.find(active) != m_defaults_palettes.end())) {
        m_active_palette = active;
    }

    // The s68 v2 paint_bindings block (reverted) is silently ignored —
    // nlohmann drops unknown keys by default.

    LOG_INFO("SwatchLibrary::load_legacy_color_system: migrated {} custom "
             "swatch(es), {} custom palette(s), {} recent(s), "
             "active_palette='{}'",
             m_custom_swatches.size(), m_custom_palettes.size(),
             m_recents.size(), m_active_palette);
}

// ── App-global defaults singleton ──────────────────────────────────────────
//
// Function-local static — same pattern as MacroManager::instance(). Loader
// reads ~/.config/curvz/swatches.json once at startup via load_app_defaults(),
// called from Application::on_activate(). The singleton has the lifetime
// of the process.
//
// Defined outside the class because it carries file-I/O dependencies
// (std::filesystem, std::fstream, Glib::get_user_config_dir) that the
// data-only SwatchLibrary header doesn't need to drag in.

SwatchLibrary& swatch_defaults() {
    static SwatchLibrary s_instance;
    return s_instance;
}

std::string app_defaults_path() {
    namespace fs = std::filesystem;
    std::string cfg = Glib::get_user_config_dir();
    return (fs::path(cfg) / "curvz" / "swatches.json").string();
}

bool load_app_defaults() {
    namespace fs = std::filesystem;
    std::string path = app_defaults_path();

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        // Not an error. Pre-1.0 the file isn't authored yet; post-1.0 a
        // fresh install may precede the config dir's creation. Either way
        // the defaults tier stays empty and the two-tier machinery falls
        // through to custom-only behaviour.
        LOG_INFO("color::load_app_defaults: '{}' not found — defaults tier "
                 "will be empty (pre-1.0 expected)", path);
        return false;
    }

    std::ifstream f(path);
    if (!f) {
        LOG_WARN("color::load_app_defaults: cannot open '{}'", path);
        return false;
    }

    nlohmann::json j;
    try { f >> j; }
    catch (const nlohmann::json::exception& e) {
        LOG_WARN("color::load_app_defaults: JSON parse error in '{}': {}",
                 path, e.what());
        return false;
    }

    swatch_defaults().from_defaults_json(j);
    LOG_INFO("color::load_app_defaults: loaded defaults from '{}'", path);
    return true;
}

} // namespace color
} // namespace Curvz
