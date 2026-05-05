#include "AppPreferences.hpp"
#include "CurvzLog.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Curvz {

AppPreferences& AppPreferences::instance() {
    static AppPreferences inst;
    return inst;
}

// ── Path resolution (matches MacroSystem::macros_path pattern) ───────────────
std::string AppPreferences::prefs_path() const {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = xdg ? xdg
                           : (std::string(std::getenv("HOME")
                                              ? std::getenv("HOME")
                                              : ".") +
                              "/.config");
    return base + "/curvz/preferences.json";
}

// ── Load ─────────────────────────────────────────────────────────────────────
// Tolerates: missing file (silent — first run), missing keys (defaults),
// wrong types (logged warning, default kept). The point is robustness
// against schema drift — never refuse to start because a pref file is old.
void AppPreferences::load() {
    const std::string path = prefs_path();
    std::ifstream f(path);
    if (!f) {
        LOG_INFO("AppPreferences: no preferences.json at {} — using defaults",
                 path);
        return;
    }

    try {
        nlohmann::json j;
        f >> j;

        // s143 m1 — boolean cleanup is a quality int 0..10.
        //   0  = most aggressive cleanup
        //   5  = default
        //   10 = no cleanup — raw Clipper2
        // Legacy boolean_cleanup_enabled bool migrates as:
        //   true  → 5  (cleanup on at default — what they asked for)
        //   false → 10 (cleanup off — raw Clipper2, what they had)
        if (j.contains("boolean_cleanup_quality") &&
            j["boolean_cleanup_quality"].is_number_integer()) {
            int q = j["boolean_cleanup_quality"].get<int>();
            if (q < 0)  q = 0;
            if (q > 10) q = 10;
            m_boolean_cleanup_quality = q;
        } else if (j.contains("boolean_cleanup_enabled") &&
                   j["boolean_cleanup_enabled"].is_boolean()) {
            m_boolean_cleanup_quality =
                j["boolean_cleanup_enabled"].get<bool>() ? 5 : 10;
            LOG_INFO("AppPreferences: migrated legacy "
                     "boolean_cleanup_enabled → quality={}",
                     m_boolean_cleanup_quality);
        }

        if (j.contains("library_defaults_seeded") &&
            j["library_defaults_seeded"].is_boolean()) {
            m_library_defaults_seeded =
                j["library_defaults_seeded"].get<bool>();
        }

        LOG_INFO("AppPreferences: loaded from {} — "
                 "boolean_cleanup_quality={}, library_defaults_seeded={}",
                 path, m_boolean_cleanup_quality,
                 m_library_defaults_seeded);
    } catch (const std::exception& e) {
        LOG_WARN("AppPreferences: failed to parse {}: {} — using defaults",
                 path, e.what());
    }
}

// ── Save ─────────────────────────────────────────────────────────────────────
void AppPreferences::save() const {
    const std::string path = prefs_path();
    try {
        fs::create_directories(fs::path(path).parent_path());
    } catch (const std::exception& e) {
        LOG_WARN("AppPreferences: cannot create config dir for {}: {}",
                 path, e.what());
        return;
    }

    nlohmann::json j;
    j["boolean_cleanup_quality"] = m_boolean_cleanup_quality;
    j["library_defaults_seeded"] = m_library_defaults_seeded;

    std::ofstream f(path);
    if (f) {
        f << j.dump(2) << "\n";
        LOG_INFO("AppPreferences: saved to {}", path);
    } else {
        LOG_WARN("AppPreferences: failed to write {}", path);
    }
}

// ── Setters ──────────────────────────────────────────────────────────────────
void AppPreferences::set_boolean_cleanup_quality(int v) {
    if (v < 0)  v = 0;
    if (v > 10) v = 10;
    if (m_boolean_cleanup_quality == v) return;
    m_boolean_cleanup_quality = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_library_defaults_seeded(bool v) {
    if (m_library_defaults_seeded == v) return;
    m_library_defaults_seeded = v;
    save();
    m_sig_changed.emit();
}

} // namespace Curvz
