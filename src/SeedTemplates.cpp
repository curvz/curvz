// ── SeedTemplates.cpp (S63 M3) ───────────────────────────────────────────────
//
// First-run seeding of Curvz's user template library, driven by JSON.
//
// The seed list lives in `seeds.json`, bundled via GResources and extracted
// to `~/.config/curvz/templates/seeds.json` on first run. The user can edit
// that file freely — add seeds, remove them, change proportions, whatever —
// and on next launch (after deleting the .seeds_v1 marker) their edits
// rebuild into actual .curvztpl/ bundles.
//
// Flow:
//   seed_defaults_if_needed()
//     ├─ if marker ~/.config/curvz/templates/.seeds_v1 exists → return
//     ├─ ensure_seeds_json_on_disk()
//     │     ├─ if user file missing → extract from GResources
//     │     └─ if extraction fails → we'll fall back to bundled content in
//     │        memory below
//     ├─ load_seeds_json()
//     │     ├─ try to read user file; on success, use it
//     │     └─ on parse error → fall back to bundled GResources content
//     │        (never leaves the user without seeds because of their typo)
//     ├─ parse into defaults + vector<SeedDescriptor>
//     ├─ for each: build_seed_doc → save()
//     └─ write marker
//
// Robustness: individual seed parse errors are logged and the seed is
// skipped, not fatal. A broken JSON doc falls back to the bundled copy.
// A seed that fails to save is logged and skipped, not fatal.

#include "CurvzDocument.hpp"
#include "CurvzLog.hpp"
#include "TemplateLibrary.hpp"
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <glibmm/miscutils.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Curvz {
namespace templates {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr const char* k_seed_marker_name  = ".seeds_v1";
static constexpr const char* k_seeds_file_name   = "seeds.json";
static constexpr const char* k_seeds_resource    = "/com/curvz/app/data/seeds.json";

// Built-in defaults used when the JSON is missing a "defaults" block entirely.
// These match the S63 house style (10 divisions, margin = half a cell,
// grid offset = half a cell).
static constexpr int    k_fallback_divisions    = 10;
static constexpr double k_fallback_margin_ratio = 0.5;
static constexpr double k_fallback_offset_ratio = 0.5;

// ── Seed descriptor (runtime) ────────────────────────────────────────────────
// One-to-one with a JSON seed object. This is the shape the builder consumes.

enum class SeedKind { Ratio, Pixel, Physical };

struct SeedDescriptor {
    std::string category;
    std::string name;
    std::string description;

    SeedKind    kind = SeedKind::Ratio;

    // Ratio mode: raw_w / raw_h / quality
    double      raw_w   = 1.0;
    double      raw_h   = 1.0;
    int         quality = 1000;

    // Pixel mode
    int         pw = 0;
    int         ph = 0;

    // Physical mode
    double      phys_w    = 0.0;
    double      phys_h    = 0.0;
    std::string phys_unit = "in";
    int         dpi       = 300;

    // Proportional rules (may be inherited from the top-level defaults)
    int    grid_divisions    = 0;
    double margin_ratio      = 0.0;
    double grid_offset_ratio = 0.0;
};

struct SeedDefaults {
    int    grid_divisions    = k_fallback_divisions;
    double margin_ratio      = k_fallback_margin_ratio;
    double grid_offset_ratio = k_fallback_offset_ratio;
};

// ── Paths ────────────────────────────────────────────────────────────────────

static std::string marker_path() {
    return (fs::path(user_dir()) / k_seed_marker_name).string();
}

static std::string user_seeds_path() {
    return (fs::path(user_dir()) / k_seeds_file_name).string();
}

static bool marker_exists() {
    std::error_code ec;
    return fs::exists(marker_path(), ec);
}

static bool write_marker() {
    std::ofstream f(marker_path());
    if (!f) return false;
    f << "Curvz seed templates version 1\n"
      << "Safe to delete this file to re-run seeding on next launch.\n"
      << "Individual seeds that already exist under the user template root\n"
      << "will not be overwritten even if the marker is removed.\n"
      << "Seed definitions live in this directory's seeds.json — edit that\n"
      << "file (and delete this marker) to customise.\n";
    return f.good();
}

// ── Load bundled seeds.json from GResources ─────────────────────────────────
// Returns the file content as a string, or empty on failure.

static std::string load_bundled_seeds_json() {
    GError* err = nullptr;
    GBytes* bytes = g_resources_lookup_data(
        k_seeds_resource, G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
    if (!bytes) {
        LOG_ERROR("SeedTemplates: bundled seeds.json not found in GResources: {}",
                  err ? err->message : "?");
        if (err) g_error_free(err);
        return {};
    }
    gsize sz = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &sz));
    std::string out(data, sz);
    g_bytes_unref(bytes);
    return out;
}

// ── First-run extraction of bundled JSON to user config dir ──────────────────
// Only writes if the user file doesn't already exist — never clobbers user
// edits. Returns true iff a file exists on disk afterward.

static bool ensure_seeds_json_on_disk() {
    std::error_code ec;
    std::string path = user_seeds_path();
    if (fs::exists(path, ec)) return true;

    std::string bundled = load_bundled_seeds_json();
    if (bundled.empty()) return false;

    // user_dir() is ensured by ensure_user_root() on scan(); redundant mkdir
    // here is cheap insurance in case seed_defaults_if_needed() is called
    // before any scan().
    fs::create_directories(user_dir(), ec);

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        LOG_WARN("SeedTemplates: failed to open '{}' for write", path);
        return false;
    }
    f.write(bundled.data(), (std::streamsize)bundled.size());
    if (!f.good()) {
        LOG_WARN("SeedTemplates: failed to write '{}'", path);
        return false;
    }
    LOG_INFO("SeedTemplates: extracted bundled seeds.json to '{}'", path);
    return true;
}

// ── Load + parse seeds.json ─────────────────────────────────────────────────
// Tries the user file first. On parse error, falls back to the bundled copy
// (so a user typo doesn't leave them without seeds). Logs clearly which
// source succeeded.

static std::optional<json> load_seeds_json() {
    // User file
    std::ifstream uf(user_seeds_path());
    if (uf) {
        json j;
        try {
            uf >> j;
            return j;
        } catch (const json::exception& e) {
            LOG_WARN("SeedTemplates: user seeds.json parse error: {} — "
                     "falling back to bundled copy", e.what());
        }
    }

    // Bundled fallback
    std::string bundled = load_bundled_seeds_json();
    if (bundled.empty()) return std::nullopt;
    try {
        return json::parse(bundled);
    } catch (const json::exception& e) {
        LOG_ERROR("SeedTemplates: BUNDLED seeds.json parse error: {}", e.what());
        return std::nullopt;
    }
}

// ── Parse a single seed object into a SeedDescriptor ────────────────────────
// Returns nullopt on any required-field error. Logs what was wrong so the
// user can find it in their edited JSON.

static std::optional<SeedDescriptor> parse_seed(const json& j,
                                                const SeedDefaults& defaults) {
    SeedDescriptor sd;

    // Required fields
    if (!j.contains("category") || !j.contains("name") || !j.contains("canvas")) {
        LOG_WARN("SeedTemplates: seed missing category/name/canvas — skipping");
        return std::nullopt;
    }
    try {
        sd.category    = j.value("category",    std::string{});
        sd.name        = j.value("name",        std::string{});
        sd.description = j.value("description", std::string{});

        // Canvas block
        const json& c = j.at("canvas");
        std::string kind = c.value("kind", std::string{});
        if (kind == "ratio") {
            sd.kind    = SeedKind::Ratio;
            sd.raw_w   = c.value("width",  1.0);
            sd.raw_h   = c.value("height", 1.0);
            sd.quality = c.value("quality", 1000);
        } else if (kind == "pixel") {
            sd.kind = SeedKind::Pixel;
            sd.pw   = c.value("width",  0);
            sd.ph   = c.value("height", 0);
            if (sd.pw <= 0 || sd.ph <= 0) {
                LOG_WARN("SeedTemplates: pixel seed '{}/{}' has invalid size "
                         "{}x{} — skipping",
                         sd.category, sd.name, sd.pw, sd.ph);
                return std::nullopt;
            }
        } else if (kind == "physical") {
            sd.kind      = SeedKind::Physical;
            sd.phys_w    = c.value("width",  0.0);
            sd.phys_h    = c.value("height", 0.0);
            sd.phys_unit = c.value("unit",   std::string("in"));
            sd.dpi       = c.value("dpi",    300);
            if (sd.phys_w <= 0.0 || sd.phys_h <= 0.0) {
                LOG_WARN("SeedTemplates: physical seed '{}/{}' has invalid "
                         "size {}x{} — skipping",
                         sd.category, sd.name, sd.phys_w, sd.phys_h);
                return std::nullopt;
            }
        } else {
            LOG_WARN("SeedTemplates: seed '{}/{}' has unknown canvas.kind "
                     "'{}' (expected ratio/pixel/physical) — skipping",
                     sd.category, sd.name, kind);
            return std::nullopt;
        }

        // Proportions: inherit defaults; override per-field if present.
        sd.grid_divisions    = defaults.grid_divisions;
        sd.margin_ratio      = defaults.margin_ratio;
        sd.grid_offset_ratio = defaults.grid_offset_ratio;
        if (j.contains("proportions")) {
            const json& p = j.at("proportions");
            sd.grid_divisions    = p.value("grid_divisions",    sd.grid_divisions);
            sd.margin_ratio      = p.value("margin_ratio",      sd.margin_ratio);
            sd.grid_offset_ratio = p.value("grid_offset_ratio", sd.grid_offset_ratio);
        }
    } catch (const json::exception& e) {
        LOG_WARN("SeedTemplates: parse error in seed '{}/{}': {} — skipping",
                 sd.category, sd.name, e.what());
        return std::nullopt;
    }

    if (sd.category.empty() || sd.name.empty()) {
        LOG_WARN("SeedTemplates: seed with empty category or name — skipping");
        return std::nullopt;
    }
    return sd;
}

// ── Parse the top-level defaults block ───────────────────────────────────────

static SeedDefaults parse_defaults(const json& root) {
    SeedDefaults d;
    if (!root.contains("defaults")) return d;
    try {
        const json& j = root.at("defaults");
        d.grid_divisions    = j.value("grid_divisions",    d.grid_divisions);
        d.margin_ratio      = j.value("margin_ratio",      d.margin_ratio);
        d.grid_offset_ratio = j.value("grid_offset_ratio", d.grid_offset_ratio);
    } catch (const json::exception& e) {
        LOG_WARN("SeedTemplates: defaults block parse error ({}) — using "
                 "built-in fallbacks", e.what());
    }
    return d;
}

// ── Builder: SeedDescriptor → CurvzDocument ──────────────────────────────────
// Single source of truth for how a descriptor becomes a doc. Used for every
// seed regardless of whether the descriptor came from the user's edited JSON
// or the bundled fallback.

static std::unique_ptr<CurvzDocument> build_seed_doc(const SeedDescriptor& sd) {
    auto doc = std::make_unique<CurvzDocument>();
    switch (sd.kind) {
    case SeedKind::Ratio:
        doc->canvas = CanvasModel::from_ratio(sd.raw_w, sd.raw_h, sd.quality);
        break;
    case SeedKind::Pixel:
        doc->canvas = CanvasModel::from_pixels(sd.pw, sd.ph);
        break;
    case SeedKind::Physical:
        doc->canvas = CanvasModel::from_physical(sd.phys_w, sd.phys_h,
                                                 sd.phys_unit, sd.dpi);
        break;
    }
    return doc;
}

// ── Public entry point ───────────────────────────────────────────────────────

void seed_defaults_if_needed() {
    if (marker_exists()) {
        return;  // already seeded on a prior run
    }

    // Extract bundled JSON to user dir if missing. Non-fatal if this fails —
    // load_seeds_json() will still try GResources directly.
    ensure_seeds_json_on_disk();

    auto root_opt = load_seeds_json();
    if (!root_opt) {
        LOG_ERROR("SeedTemplates: no usable seeds.json — skipping seeding");
        // Don't write the marker in this case — we want to retry next launch
        // in case the user fixes their JSON.
        return;
    }
    const json& root = *root_opt;

    const SeedDefaults defaults = parse_defaults(root);

    if (!root.contains("seeds") || !root.at("seeds").is_array()) {
        LOG_ERROR("SeedTemplates: seeds.json has no 'seeds' array");
        return;
    }

    const json& arr = root.at("seeds");
    LOG_INFO("SeedTemplates: first-run seeding ({} entries in JSON)",
             arr.size());

    int written = 0;
    int skipped = 0;
    int failed  = 0;

    for (const auto& jseed : arr) {
        if (!jseed.is_object()) continue;  // tolerate comment-style entries
        // Tolerate entries that are comment-only (just "_comment" with no
        // name/category). Used in the bundled JSON to label category sections.
        if (!jseed.contains("name") || !jseed.contains("category")) continue;

        auto sd_opt = parse_seed(jseed, defaults);
        if (!sd_opt) { ++failed; continue; }
        const SeedDescriptor& sd = *sd_opt;

        if (user_bundle_exists(sd.category, sd.name)) {
            ++skipped;
            continue;
        }

        auto doc = build_seed_doc(sd);
        if (!doc) {
            LOG_WARN("SeedTemplates: build_seed_doc failed for '{}/{}'",
                     sd.category, sd.name);
            ++failed;
            continue;
        }

        TemplateMeta meta;
        meta.name              = sd.name;
        meta.category          = sd.category;
        meta.description       = sd.description;
        meta.author            = "Curvz";
        meta.grid_divisions    = sd.grid_divisions;
        meta.margin_ratio      = sd.margin_ratio;
        meta.grid_offset_ratio = sd.grid_offset_ratio;

        // m4: save() now writes both motif PNGs eagerly and needs both
        // motifs' workspace+artboard colours. First-run seeding has no
        // project context, so we use the canonical defaults baked into
        // CurvzProject (artboard_dark = 0.157, workspace_dark = 0.09,
        // artboard_light = 0.878, workspace_light = 0.753). If a user
        // later customises their project's motif colours, that's fine —
        // the seed PNGs are still valid; they just don't reflect the
        // post-customise palette. (User regenerates by deleting the
        // bundle's PNG; ensure_thumb_for_motif rebuilds it on next view.)
        if (!save(*doc, meta,
                  /*dark workspace*/  0.09,  0.09,  0.09,
                  /*dark artboard*/   0.157, 0.157, 0.157,
                  /*light workspace*/ 0.753, 0.753, 0.753,
                  /*light artboard*/  0.878, 0.878, 0.878)) {
            LOG_WARN("SeedTemplates: save failed for '{}/{}'",
                     sd.category, sd.name);
            ++failed;
            continue;
        }
        ++written;
    }

    if (!write_marker()) {
        LOG_WARN("SeedTemplates: failed to write marker '{}'", marker_path());
    }

    LOG_INFO("SeedTemplates: seeded {} templates ({} skipped, {} failed)",
             written, skipped, failed);
}

} // namespace templates
} // namespace Curvz
