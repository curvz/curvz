//
// ThemeIO.cpp — bridge implementation (S103 m2).
//
// Three layers' worth of work fits in this one file:
//   1. File-level read / write of the JSON envelope (anonymous-namespace
//      helpers).
//   2. The seven public bridge entry points declared in ThemeIO.hpp.
//
// The format pumps for a single Theme (theme_to_json / theme_from_json)
// live on ThemeLibrary.cpp at namespace theme:: scope — they're shared
// with the project-tier round-trip so format additions to a Theme only
// have to update one place. Same lift-out-of-anon move as S102 m1's
// style_to_json / style_from_json refactor.
//

#include "theme/ThemeIO.hpp"
#include "theme/ThemeLibrary.hpp"          // theme_to_json / theme_from_json
#include "CommandHistory.hpp"              // AddThemeCommand, CompositeCommand
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
namespace theme {
namespace theme_io {

namespace {

using json = nlohmann::json;

// ── Envelope shape ──────────────────────────────────────────────────
//
// {
//   "version": 1,
//   "themes":  [ {<theme_to_json output>}, ... ]
// }
//
// "version" is permissive on read (missing → 1). "themes" is the
// authoritative payload; missing or non-array → file is treated as
// empty (caller decides whether to warn). Foreign top-level keys are
// silently ignored — same tolerance posture as the project.json
// loader and StyleIO.
constexpr int kCurrentVersion = 1;

// Read a JSON file from disk into a parsed json value. Returns nullopt
// on missing file, I/O error, or parse error. Logs the specific cause.
std::optional<json> read_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        LOG_WARN("ThemeIO: cannot open '{}' for reading", path);
        return std::nullopt;
    }
    try {
        json j;
        in >> j;
        return j;
    } catch (const std::exception& e) {
        LOG_WARN("ThemeIO: parse error in '{}': {}", path, e.what());
        return std::nullopt;
    }
}

// Write a JSON value to disk with pretty-printing (2-space indent).
// Returns false on I/O error.
bool write_json_file(const std::string& path, const json& j) {
    std::ofstream out(path);
    if (!out) {
        LOG_WARN("ThemeIO: cannot open '{}' for writing", path);
        return false;
    }
    try {
        out << j.dump(2);
        out.put('\n');
        return out.good();
    } catch (const std::exception& e) {
        LOG_WARN("ThemeIO: write error to '{}': {}", path, e.what());
        return false;
    }
}

// Decode an envelope into a vector<Theme>. Returns nullopt only if the
// envelope's top-level shape is invalid (not an object, missing
// "themes" array). An empty / all-bad-entries file returns an empty
// vector successfully — that's a legitimate decode, just not a
// useful one.
std::optional<std::vector<Theme>> decode_envelope(const json& j) {
    if (!j.is_object()) {
        LOG_WARN("ThemeIO: envelope is not a JSON object");
        return std::nullopt;
    }
    if (!j.contains("themes") || !j["themes"].is_array()) {
        LOG_WARN("ThemeIO: envelope missing 'themes' array");
        return std::nullopt;
    }

    // Version is informational at v1 — log a divergence so the user
    // knows if they're loading a file that's newer than the build.
    int version = j.value("version", kCurrentVersion);
    if (version != kCurrentVersion) {
        LOG_INFO("ThemeIO: envelope version {} (build expects {}); "
                 "attempting load anyway", version, kCurrentVersion);
    }

    std::vector<Theme> out;
    std::size_t skipped = 0;
    for (const auto& entry : j["themes"]) {
        auto t = theme_from_json(entry);
        if (!t) { ++skipped; continue; }
        // Defensive: theme_from_json already drops "app:" ids, but
        // belt-and-braces — re-check at the bridge layer too.
        if (theme::is_built_in(t->header.id)) {
            ++skipped;
            continue;
        }
        out.push_back(std::move(*t));
    }

    if (skipped > 0) {
        LOG_INFO("ThemeIO: decoded {} themes, skipped {} bad entries",
                 out.size(), skipped);
    }
    return out;
}

// Encode a vector<Theme> into the envelope JSON.
json encode_envelope(const std::vector<Theme>& themes) {
    json arr = json::array();
    for (const Theme& t : themes) {
        arr.push_back(theme_to_json(t));
    }
    json out;
    out["version"] = kCurrentVersion;
    out["themes"]  = std::move(arr);
    return out;
}

// Disambiguate `name` against `taken` by appending " 2", " 3", … until
// no collision. Caps at 999 with a warn log to avoid pathological
// loops on a saturated name space (in practice unreachable).
std::string disambiguate(const std::string&                     name,
                         const std::unordered_set<std::string>& taken) {
    if (taken.find(name) == taken.end()) return name;
    for (int n = 2; n <= 999; ++n) {
        std::string candidate = name + " " + std::to_string(n);
        if (taken.find(candidate) == taken.end()) return candidate;
    }
    LOG_WARN("ThemeIO: name disambiguation gave up on '{}' after 999 "
             "tries; using base name", name);
    return name;
}

} // anon namespace

// ── Public API ───────────────────────────────────────────────────────

std::string user_themes_dir() {
    namespace fs = std::filesystem;
    return (fs::path(Glib::get_user_config_dir()) / "curvz" /
            "themes").string();
}

std::vector<std::string> enumerate_user() {
    namespace fs = std::filesystem;
    std::vector<std::string> out;

    const std::string dir = user_themes_dir();
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
        LOG_WARN("ThemeIO::enumerate_user: directory read error in '{}': {}",
                 dir, ec.message());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::vector<Theme>> load_user(const std::string& stem) {
    namespace fs = std::filesystem;
    if (stem.empty()) return std::nullopt;
    const std::string path =
        (fs::path(user_themes_dir()) / (stem + ".json")).string();
    return load_path(path);
}

std::optional<std::vector<Theme>> load_path(const std::string& path) {
    auto j = read_json_file(path);
    if (!j) return std::nullopt;
    return decode_envelope(*j);
}

bool write_path(const std::string& path, const std::vector<Theme>& themes) {
    return write_json_file(path, encode_envelope(themes));
}

std::vector<Theme> snapshot_user_tier(const ThemeLibrary& lib) {
    // Walk every user category, pull every theme. Built-ins are
    // omitted by construction (we never iterate the app list).
    std::vector<Theme> out;
    for (const std::string& cat : lib.user_categories()) {
        for (const Theme* t : lib.user_themes_in_category(cat)) {
            if (t) out.push_back(*t);
        }
    }
    return out;
}

std::size_t import_themes_into_library(ThemeLibrary&             lib,
                                       CommandHistory*           history,
                                       const std::vector<Theme>& incoming) {
    if (incoming.empty()) return 0;

    // Build the existing-name set ONCE, then update it as we mint new
    // imports — disambiguation needs to be consistent across the batch
    // (importing two files both named "My Theme" produces "My Theme"
    // and "My Theme 2", not two "My Theme"s).
    std::unordered_set<std::string> taken;
    for (const std::string& cat : lib.user_categories()) {
        for (const Theme* t : lib.user_themes_in_category(cat)) {
            if (t) taken.insert(t->header.name);
        }
    }

    // No history → fall back to direct add. The dialog always passes a
    // real history; this branch is for tests / future non-UI callers.
    if (!history) {
        std::size_t n = 0;
        for (const Theme& src : incoming) {
            Theme copy = src;
            // Strip foreign id; let the library mint a fresh thm_<uuid>.
            copy.header.id = "";
            copy.header.name = disambiguate(
                copy.header.name.empty() ? std::string("Imported theme")
                                         : copy.header.name,
                taken);
            taken.insert(copy.header.name);
            ThemeId new_id = lib.add_theme(std::move(copy));
            if (!new_id.empty()) ++n;
        }
        return n;
    }

    // History path — wrap every add in one CompositeCommand. Each
    // AddThemeCommand mints its own id at execute() time (via library)
    // because we leave header.id empty. That means in a "do, undo,
    // redo" cycle, the redoes target the same minted ids that the
    // first execute captured into m_assigned_id — see AddThemeCommand
    // doc for the full id-stability story.
    auto composite = std::make_unique<CompositeCommand>(
        incoming.size() == 1
            ? std::string("Import theme")
            : std::string("Import ") + std::to_string(incoming.size())
                  + " themes");

    std::size_t added = 0;
    for (const Theme& src : incoming) {
        Theme copy = src;
        copy.header.id = "";
        copy.header.name = disambiguate(
            copy.header.name.empty() ? std::string("Imported theme")
                                     : copy.header.name,
            taken);
        taken.insert(copy.header.name);
        composite->add(std::make_unique<AddThemeCommand>(
            &lib, std::move(copy), "Import theme"));
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

} // namespace theme_io
} // namespace theme
} // namespace Curvz
