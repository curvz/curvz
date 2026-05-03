//
// StyleIO.cpp — bridge implementation (S102 m1).
//
// Three layers' worth of work fits in this one file:
//   1. File-level read / write of the JSON envelope (anonymous-namespace
//      helpers).
//   2. The five public bridge entry points declared in StyleIO.hpp.
//
// The format pumps for a single Style (style_to_json / style_from_json)
// live on StyleLibrary.cpp at namespace style:: scope — they're shared
// with the project-tier round-trip so format additions to a Style only
// have to update one place.
//

#include "style/StyleIO.hpp"
#include "style/StyleLibrary.hpp"          // style_to_json / style_from_json
#include "CommandHistory.hpp"              // AddStyleCommand, CompositeCommand
#include "SceneNode.hpp"                   // generate_internal_id (UUID)
#include "CurvzLog.hpp"

#include <glibmm/miscutils.h>              // Glib::get_user_config_dir
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

namespace Curvz {
namespace style {
namespace style_io {

namespace {

using json = nlohmann::json;

// ── Envelope shape ──────────────────────────────────────────────────
//
// {
//   "version": 1,
//   "styles":  [ {<style_to_json output>}, ... ]
// }
//
// "version" is permissive on read (missing → 1). "styles" is the
// authoritative payload; missing or non-array → file is treated as
// empty (caller decides whether to warn). Foreign top-level keys are
// silently ignored — same tolerance posture as the project.json
// loader.
constexpr int kCurrentVersion = 1;

// Read a JSON file from disk into a parsed json value. Returns nullopt
// on missing file, I/O error, or parse error. Logs the specific cause.
std::optional<json> read_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        LOG_WARN("StyleIO: cannot open '{}' for reading", path);
        return std::nullopt;
    }
    try {
        json j;
        in >> j;
        return j;
    } catch (const std::exception& e) {
        LOG_WARN("StyleIO: parse error in '{}': {}", path, e.what());
        return std::nullopt;
    }
}

// Write a JSON value to disk with pretty-printing (2-space indent).
// Returns false on I/O error.
bool write_json_file(const std::string& path, const json& j) {
    std::ofstream out(path);
    if (!out) {
        LOG_WARN("StyleIO: cannot open '{}' for writing", path);
        return false;
    }
    try {
        out << j.dump(2);
        out.put('\n');
        return out.good();
    } catch (const std::exception& e) {
        LOG_WARN("StyleIO: write error to '{}': {}", path, e.what());
        return false;
    }
}

// Decode an envelope into a vector<Style>. Returns nullopt only if the
// envelope's top-level shape is invalid (not an object, missing
// "styles" array). An empty / all-bad-entries file returns an empty
// vector successfully — that's a legitimate decode, just not a
// useful one.
std::optional<std::vector<Style>> decode_envelope(const json& j) {
    if (!j.is_object()) {
        LOG_WARN("StyleIO: envelope is not a JSON object");
        return std::nullopt;
    }
    if (!j.contains("styles") || !j["styles"].is_array()) {
        LOG_WARN("StyleIO: envelope missing 'styles' array");
        return std::nullopt;
    }

    // Version is informational at v1 — we just log a divergence so the
    // user knows if they're loading a file that's newer than the build.
    int version = j.value("version", kCurrentVersion);
    if (version != kCurrentVersion) {
        LOG_INFO("StyleIO: envelope version {} (build expects {}); "
                 "attempting load anyway", version, kCurrentVersion);
    }

    std::vector<Style> out;
    std::size_t skipped = 0;
    for (const auto& entry : j["styles"]) {
        auto s = style_from_json(entry);
        if (!s) { ++skipped; continue; }
        // Defensive: style_from_json already drops "app:" ids, but
        // belt-and-braces — re-check at the bridge layer too.
        if (style::is_built_in(s->header.id)) {
            ++skipped;
            continue;
        }
        out.push_back(std::move(*s));
    }

    if (skipped > 0) {
        LOG_INFO("StyleIO: decoded {} styles, skipped {} bad entries",
                 out.size(), skipped);
    }
    return out;
}

// Encode a vector<Style> into the envelope JSON.
json encode_envelope(const std::vector<Style>& styles) {
    json arr = json::array();
    for (const Style& s : styles) {
        arr.push_back(style_to_json(s));
    }
    json out;
    out["version"] = kCurrentVersion;
    out["styles"]  = std::move(arr);
    return out;
}

// Disambiguate `name` against `taken` by appending " 2", " 3", … until
// no collision. Caps at 999 with a warn log to avoid pathological
// loops on a saturated name space (in practice unreachable — the user
// tier is sub-100 entries Phase 1).
std::string disambiguate(const std::string&                  name,
                         const std::unordered_set<std::string>& taken) {
    if (taken.find(name) == taken.end()) return name;
    for (int n = 2; n <= 999; ++n) {
        std::string candidate = name + " " + std::to_string(n);
        if (taken.find(candidate) == taken.end()) return candidate;
    }
    LOG_WARN("StyleIO: name disambiguation gave up on '{}' after 999 "
             "tries; using base name", name);
    return name;
}

} // anon namespace

// ── Public API ───────────────────────────────────────────────────────

std::string user_styles_dir() {
    namespace fs = std::filesystem;
    return (fs::path(Glib::get_user_config_dir()) / "curvz" /
            "styles").string();
}

std::vector<std::string> enumerate_user() {
    namespace fs = std::filesystem;
    std::vector<std::string> out;

    const std::string dir = user_styles_dir();
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        // Directory only exists after the first export. Empty result
        // is the correct answer pre-bootstrap.
        return out;
    }

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& path = entry.path();
        if (path.extension() != ".json") continue;
        out.push_back(path.stem().string());
    }
    if (ec) {
        LOG_WARN("StyleIO::enumerate_user: directory read error in '{}': {}",
                 dir, ec.message());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::vector<Style>> load_user(const std::string& stem) {
    namespace fs = std::filesystem;
    if (stem.empty()) return std::nullopt;
    const std::string path =
        (fs::path(user_styles_dir()) / (stem + ".json")).string();
    return load_path(path);
}

std::optional<std::vector<Style>> load_path(const std::string& path) {
    auto j = read_json_file(path);
    if (!j) return std::nullopt;
    return decode_envelope(*j);
}

bool write_path(const std::string& path, const std::vector<Style>& styles) {
    return write_json_file(path, encode_envelope(styles));
}

std::vector<Style> snapshot_user_tier(const StyleLibrary& lib) {
    // Walk every user category, pull every style. Built-ins are
    // omitted by construction (we never iterate the app list).
    std::vector<Style> out;
    for (const std::string& cat : lib.user_categories()) {
        for (const Style* s : lib.user_styles_in_category(cat)) {
            if (s) out.push_back(*s);
        }
    }
    return out;
}

std::size_t import_styles_into_library(StyleLibrary&             lib,
                                       CommandHistory*           history,
                                       const std::vector<Style>& incoming) {
    if (incoming.empty()) return 0;

    // Build the existing-name set ONCE, then update it as we mint new
    // imports — disambiguation needs to be consistent across the batch
    // (importing two files both named "My Style" produces "My Style"
    // and "My Style 2", not two "My Style"s).
    std::unordered_set<std::string> taken;
    for (const std::string& cat : lib.user_categories()) {
        for (const Style* s : lib.user_styles_in_category(cat)) {
            if (s) taken.insert(s->header.name);
        }
    }

    // No history → fall back to direct add. The panel always passes a
    // real history; this branch is for tests / future non-UI callers.
    if (!history) {
        std::size_t n = 0;
        for (const Style& src : incoming) {
            Style copy = src;
            // Strip foreign id; let the library mint a fresh stl_<uuid>.
            copy.header.id = "";
            copy.header.name = disambiguate(
                copy.header.name.empty() ? std::string("Imported style")
                                         : copy.header.name,
                taken);
            taken.insert(copy.header.name);
            StyleId new_id = lib.add_style(std::move(copy));
            if (!new_id.empty()) ++n;
        }
        return n;
    }

    // History path — wrap every add in one CompositeCommand. Each
    // AddStyleCommand mints its own id at execute() time (via library)
    // because we leave header.id empty. That means in a "do, undo,
    // redo" cycle, the redoes target the same minted ids that the
    // first execute captured into m_assigned_id — see AddStyleCommand
    // doc for the full id-stability story.
    auto composite = std::make_unique<CompositeCommand>(
        incoming.size() == 1
            ? std::string("Import style")
            : std::string("Import ") + std::to_string(incoming.size())
                  + " styles");

    std::size_t added = 0;
    for (const Style& src : incoming) {
        Style copy = src;
        copy.header.id = "";
        copy.header.name = disambiguate(
            copy.header.name.empty() ? std::string("Imported style")
                                     : copy.header.name,
            taken);
        taken.insert(copy.header.name);
        composite->add(std::make_unique<AddStyleCommand>(
            &lib, std::move(copy), "Import style"));
        ++added;
    }

    if (added == 0) {
        // No commands to push — composite would be a no-op execute /
        // no-op undo, which clutters the history. Skip.
        return 0;
    }

    // Codebase convention: caller executes, then pushes onto the
    // history. The history's push() does NOT execute (it just records).
    composite->execute();
    history->push(std::move(composite));
    return added;
}

} // namespace style_io
} // namespace style
} // namespace Curvz
