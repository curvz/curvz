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

        if (j.contains("boolean_cleanup_enabled") &&
            j["boolean_cleanup_enabled"].is_boolean()) {
            m_boolean_cleanup_enabled = j["boolean_cleanup_enabled"].get<bool>();
        }

        if (j.contains("boolean_reduce_enabled") &&
            j["boolean_reduce_enabled"].is_boolean()) {
            m_boolean_reduce_enabled = j["boolean_reduce_enabled"].get<bool>();
        }

        LOG_INFO("AppPreferences: loaded from {} — "
                 "boolean_cleanup_enabled={}, boolean_reduce_enabled={}",
                 path, m_boolean_cleanup_enabled, m_boolean_reduce_enabled);
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
    j["boolean_cleanup_enabled"] = m_boolean_cleanup_enabled;
    j["boolean_reduce_enabled"]  = m_boolean_reduce_enabled;

    std::ofstream f(path);
    if (f) {
        f << j.dump(2) << "\n";
        LOG_INFO("AppPreferences: saved to {}", path);
    } else {
        LOG_WARN("AppPreferences: failed to write {}", path);
    }
}

// ── Setters ──────────────────────────────────────────────────────────────────
void AppPreferences::set_boolean_cleanup_enabled(bool v) {
    if (m_boolean_cleanup_enabled == v) return;
    m_boolean_cleanup_enabled = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_boolean_reduce_enabled(bool v) {
    if (m_boolean_reduce_enabled == v) return;
    m_boolean_reduce_enabled = v;
    save();
    m_sig_changed.emit();
}

} // namespace Curvz
