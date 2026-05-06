#include "RecentProjects.hpp"
#include "AppPreferences.hpp"
#include "CurvzLog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Curvz {

RecentProjects& RecentProjects::instance() {
    static RecentProjects inst;
    return inst;
}

// ── Path resolution (matches AppPreferences::prefs_path pattern) ─────────────
std::string RecentProjects::recents_path() const {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = xdg ? xdg
                           : (std::string(std::getenv("HOME")
                                              ? std::getenv("HOME")
                                              : ".") +
                              "/.config");
    return base + "/curvz/recent_projects.json";
}

// ── Normalise ────────────────────────────────────────────────────────────────
// Strip trailing slashes so /foo/bar.curvz/ and /foo/bar.curvz dedupe.
// Deliberately *not* canonicalising symlinks — the recents menu should show
// the path the user opened by, not its resolved target.
std::string RecentProjects::normalise(const std::string& path) {
    std::string s = path;
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

// ── Load ─────────────────────────────────────────────────────────────────────
void RecentProjects::load() {
    const std::string path = recents_path();
    std::ifstream f(path);
    if (!f) {
        LOG_INFO("RecentProjects: no recent_projects.json at {} — empty list",
                 path);
        return;
    }

    try {
        nlohmann::json j;
        f >> j;

        m_paths.clear();
        if (j.contains("paths") && j["paths"].is_array()) {
            for (const auto& entry : j["paths"]) {
                if (!entry.is_string()) continue;
                std::string p = normalise(entry.get<std::string>());
                if (p.empty()) continue;
                // Drop entries that no longer exist on disk. This is the
                // single point where stale recents disappear; the menu
                // builder trusts the in-memory list afterwards.
                std::error_code ec;
                if (!fs::exists(p, ec)) {
                    LOG_INFO("RecentProjects: dropping stale entry '{}'", p);
                    continue;
                }
                // Dedupe — paths.json should already be unique, but tolerate
                // hand-edited or older-version files.
                if (std::find(m_paths.begin(), m_paths.end(), p)
                    != m_paths.end()) continue;
                m_paths.push_back(std::move(p));
            }
        }

        LOG_INFO("RecentProjects: loaded {} entr{} from {}",
                 m_paths.size(), m_paths.size() == 1 ? "y" : "ies", path);
    } catch (const std::exception& e) {
        LOG_WARN("RecentProjects: failed to parse {}: {} — empty list",
                 path, e.what());
        m_paths.clear();
    }

    m_sig_changed.emit();
}

// ── Save ─────────────────────────────────────────────────────────────────────
void RecentProjects::save() const {
    const std::string path = recents_path();
    try {
        fs::create_directories(fs::path(path).parent_path());
    } catch (const std::exception& e) {
        LOG_WARN("RecentProjects: cannot create config dir for {}: {}",
                 path, e.what());
        return;
    }

    nlohmann::json j;
    j["paths"] = m_paths;

    std::ofstream f(path);
    if (f) {
        f << j.dump(2) << "\n";
    } else {
        LOG_WARN("RecentProjects: failed to write {}", path);
    }
}

// ── Add ──────────────────────────────────────────────────────────────────────
// Move-to-front semantics: if path is already in the list, it's removed
// from its current position and re-inserted at index 0. Trim to max_count
// from the tail. Per-add save keeps disk in sync without a debounce —
// recents are written rarely (once per project open) and the file is small.
void RecentProjects::add(const std::string& path) {
    if (path.empty()) return;
    std::string key = normalise(path);
    if (key.empty()) return;

    // Remove any existing occurrence (move-to-front).
    auto it = std::find(m_paths.begin(), m_paths.end(), key);
    if (it != m_paths.end()) m_paths.erase(it);

    // Insert at front.
    m_paths.insert(m_paths.begin(), std::move(key));

    // Trim. Defensive default of 10 if pref ever returns nonsense.
    int max_count = AppPreferences::instance().recent_projects_max_count();
    if (max_count < 1)  max_count = 1;
    if (max_count > 50) max_count = 50;
    if (static_cast<int>(m_paths.size()) > max_count)
        m_paths.resize(max_count);

    save();
    m_sig_changed.emit();
    LOG_INFO("RecentProjects: added '{}' (list size {})", path, m_paths.size());
}

// ── Remove ───────────────────────────────────────────────────────────────────
void RecentProjects::remove(const std::string& path) {
    std::string key = normalise(path);
    auto it = std::find(m_paths.begin(), m_paths.end(), key);
    if (it == m_paths.end()) return;
    m_paths.erase(it);
    save();
    m_sig_changed.emit();
    LOG_INFO("RecentProjects: removed '{}' (list size {})", path,
             m_paths.size());
}

// ── Clear ────────────────────────────────────────────────────────────────────
void RecentProjects::clear() {
    m_paths.clear();
    save();
    m_sig_changed.emit();
    LOG_INFO("RecentProjects: cleared");
}

} // namespace Curvz
