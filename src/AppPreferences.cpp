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

        if (j.contains("reopen_last_project") &&
            j["reopen_last_project"].is_boolean()) {
            m_reopen_last_project = j["reopen_last_project"].get<bool>();
        }

        if (j.contains("recent_projects_max_count") &&
            j["recent_projects_max_count"].is_number_integer()) {
            int n = j["recent_projects_max_count"].get<int>();
            if (n < 1)  n = 1;
            if (n > 50) n = 50;
            m_recent_projects_max_count = n;
        }

        if (j.contains("show_rulers_by_default") &&
            j["show_rulers_by_default"].is_boolean()) {
            m_show_rulers_by_default =
                j["show_rulers_by_default"].get<bool>();
        }

        if (j.contains("undo_history_depth") &&
            j["undo_history_depth"].is_number_integer()) {
            int n = j["undo_history_depth"].get<int>();
            if (n < 50)    n = 50;
            if (n > 10000) n = 10000;
            m_undo_history_depth = n;
        }

        if (j.contains("tooltip_delay_ms") &&
            j["tooltip_delay_ms"].is_number_integer()) {
            int n = j["tooltip_delay_ms"].get<int>();
            if (n < 0)    n = 0;
            if (n > 2000) n = 2000;
            m_tooltip_delay_ms = n;
        }

        // s146 m3 — warp defaults. Clamp at load to defend against
        // hand-edited preferences.json with out-of-range values.
        if (j.contains("warp_default_top_count") &&
            j["warp_default_top_count"].is_number_integer()) {
            int n = j["warp_default_top_count"].get<int>();
            if (n < 2) n = 2;
            if (n > 4) n = 4;
            m_warp_default_top_count = n;
        }
        if (j.contains("warp_default_bot_count") &&
            j["warp_default_bot_count"].is_number_integer()) {
            int n = j["warp_default_bot_count"].get<int>();
            if (n < 2) n = 2;
            if (n > 4) n = 4;
            m_warp_default_bot_count = n;
        }
        if (j.contains("warp_default_preset") &&
            j["warp_default_preset"].is_number_integer()) {
            int n = j["warp_default_preset"].get<int>();
            if (n < 0) n = 0;
            if (n > 7) n = 7;
            m_warp_default_preset = n;
        }
        if (j.contains("warp_default_quality") &&
            j["warp_default_quality"].is_number_integer()) {
            int n = j["warp_default_quality"].get<int>();
            if (n < 1)  n = 1;
            if (n > 16) n = 16;
            m_warp_default_quality = n;
        }

        // s145 m4 — path overrides. All four are plain strings; empty
        // means "use the built-in default at the consumer." We accept
        // any string the user has saved without validation here; if
        // the path is bogus, the consumer will fail to read/write at
        // its actual access site and report there.
        if (j.contains("library_path_override") &&
            j["library_path_override"].is_string()) {
            m_library_path_override = j["library_path_override"].get<std::string>();
        }
        if (j.contains("templates_path_override") &&
            j["templates_path_override"].is_string()) {
            m_templates_path_override = j["templates_path_override"].get<std::string>();
        }
        if (j.contains("log_path_override") &&
            j["log_path_override"].is_string()) {
            m_log_path_override = j["log_path_override"].get<std::string>();
        }
        if (j.contains("custom_css_path_override") &&
            j["custom_css_path_override"].is_string()) {
            m_custom_css_path_override = j["custom_css_path_override"].get<std::string>();
        }

        if (j.contains("library_defaults_seeded") &&
            j["library_defaults_seeded"].is_boolean()) {
            m_library_defaults_seeded =
                j["library_defaults_seeded"].get<bool>();
        }

        // s152 — toolbar density. Out-of-range values fall back to
        // the default (Standard = 1) silently.
        if (j.contains("toolbar_density") &&
            j["toolbar_density"].is_number_integer()) {
            int n = j["toolbar_density"].get<int>();
            if (n < 0 || n > 3) {
                LOG_WARN("AppPreferences: toolbar_density out of range "
                         "({}), using default", n);
                n = 1;
            }
            m_toolbar_density = n;
        }

        // s202 m6 — quick-jump pick counters. JSON shape is a flat
        // object: { "0:Canvas": 7, "1:Selection": 23, ... } where
        // the key is "<phase_int>:<section_title>". Tolerate missing
        // / non-object / non-integer-value entries — defaults to
        // empty map, fresh counters.
        if (j.contains("quick_jump_counts") &&
            j["quick_jump_counts"].is_object()) {
            m_quick_jump_counts.clear();
            for (auto it = j["quick_jump_counts"].begin();
                 it != j["quick_jump_counts"].end(); ++it) {
                const std::string& key = it.key();
                if (!it.value().is_number_integer()) continue;
                int count = it.value().get<int>();
                if (count < 0) continue;
                // Parse "<phase>:<title>".
                auto colon = key.find(':');
                if (colon == std::string::npos) continue;
                int phase = 0;
                try { phase = std::stoi(key.substr(0, colon)); }
                catch (...) { continue; }
                if (phase < 0 || phase > 9) continue;  // tighter than necessary
                std::string title = key.substr(colon + 1);
                if (title.empty()) continue;
                m_quick_jump_counts[{phase, title}] = count;
            }
        }

        LOG_INFO("AppPreferences: loaded from {} — "
                 "boolean_cleanup_quality={}, reopen_last_project={}, "
                 "recent_projects_max_count={}, show_rulers_by_default={}, "
                 "undo_history_depth={}, tooltip_delay_ms={}, "
                 "library_override={}, templates_override={}, "
                 "log_override={}, css_override={}, "
                 "library_defaults_seeded={}, toolbar_density={}",
                 path, m_boolean_cleanup_quality,
                 m_reopen_last_project, m_recent_projects_max_count,
                 m_show_rulers_by_default, m_undo_history_depth,
                 m_tooltip_delay_ms,
                 m_library_path_override.empty() ? "<default>" : m_library_path_override,
                 m_templates_path_override.empty() ? "<default>" : m_templates_path_override,
                 m_log_path_override.empty() ? "<default>" : m_log_path_override,
                 m_custom_css_path_override.empty() ? "<default>" : m_custom_css_path_override,
                 m_library_defaults_seeded,
                 m_toolbar_density);
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
    j["boolean_cleanup_quality"]    = m_boolean_cleanup_quality;
    j["reopen_last_project"]        = m_reopen_last_project;
    j["recent_projects_max_count"]  = m_recent_projects_max_count;
    j["show_rulers_by_default"]     = m_show_rulers_by_default;
    j["undo_history_depth"]         = m_undo_history_depth;
    j["tooltip_delay_ms"]           = m_tooltip_delay_ms;
    j["warp_default_top_count"]     = m_warp_default_top_count;
    j["warp_default_bot_count"]     = m_warp_default_bot_count;
    j["warp_default_preset"]        = m_warp_default_preset;
    j["warp_default_quality"]       = m_warp_default_quality;
    j["library_path_override"]      = m_library_path_override;
    j["templates_path_override"]    = m_templates_path_override;
    j["log_path_override"]          = m_log_path_override;
    j["custom_css_path_override"]   = m_custom_css_path_override;
    j["library_defaults_seeded"]    = m_library_defaults_seeded;
    j["toolbar_density"]            = m_toolbar_density;

    // s202 m6 — quick-jump pick counters. Flat object keyed
    // "<phase>:<title>". Omitting when empty would save the file
    // smaller but the always-present key documents the feature
    // at a glance for anyone reading the JSON.
    nlohmann::json qjc = nlohmann::json::object();
    for (const auto& kv : m_quick_jump_counts) {
        if (kv.second <= 0) continue;  // never persist zeros
        std::string key = std::to_string(kv.first.first) + ":" +
                          kv.first.second;
        qjc[key] = kv.second;
    }
    j["quick_jump_counts"] = qjc;

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

void AppPreferences::set_reopen_last_project(bool v) {
    if (m_reopen_last_project == v) return;
    m_reopen_last_project = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_recent_projects_max_count(int v) {
    if (v < 1)  v = 1;
    if (v > 50) v = 50;
    if (m_recent_projects_max_count == v) return;
    m_recent_projects_max_count = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_show_rulers_by_default(bool v) {
    if (m_show_rulers_by_default == v) return;
    m_show_rulers_by_default = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_undo_history_depth(int v) {
    if (v < 50)    v = 50;
    if (v > 10000) v = 10000;
    if (m_undo_history_depth == v) return;
    m_undo_history_depth = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_tooltip_delay_ms(int v) {
    if (v < 0)    v = 0;
    if (v > 2000) v = 2000;
    if (m_tooltip_delay_ms == v) return;
    m_tooltip_delay_ms = v;
    save();
    m_sig_changed.emit();
}

// s146 m3 — warp creation defaults. Same clamp-or-noop-or-save shape
// as every other int pref. signal_changed lets the inspector re-read
// the value if a different surface (or another inspector instance)
// changed it.
void AppPreferences::set_warp_default_top_count(int v) {
    if (v < 2) v = 2;
    if (v > 4) v = 4;
    if (m_warp_default_top_count == v) return;
    m_warp_default_top_count = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_warp_default_bot_count(int v) {
    if (v < 2) v = 2;
    if (v > 4) v = 4;
    if (m_warp_default_bot_count == v) return;
    m_warp_default_bot_count = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_warp_default_preset(int v) {
    if (v < 0) v = 0;
    if (v > 7) v = 7;
    if (m_warp_default_preset == v) return;
    m_warp_default_preset = v;
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_warp_default_quality(int v) {
    if (v < 1)  v = 1;
    if (v > 16) v = 16;
    if (m_warp_default_quality == v) return;
    m_warp_default_quality = v;
    save();
    m_sig_changed.emit();
}

// s145 m4 — path-override setters share a small trim-and-store shape.
// Whitespace-trimming protects against pasted paths with stray spaces
// or trailing newlines from terminal copy. An all-whitespace value
// collapses to empty (= "use default"), which is what users mean if
// they typed only spaces.
namespace {
    std::string trim_path(const std::string& s) {
        const char* ws = " \t\r\n";
        const auto first = s.find_first_not_of(ws);
        if (first == std::string::npos) return {};
        const auto last = s.find_last_not_of(ws);
        return s.substr(first, last - first + 1);
    }
}

void AppPreferences::set_library_path_override(const std::string& v) {
    std::string t = trim_path(v);
    if (m_library_path_override == t) return;
    m_library_path_override = std::move(t);
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_templates_path_override(const std::string& v) {
    std::string t = trim_path(v);
    if (m_templates_path_override == t) return;
    m_templates_path_override = std::move(t);
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_log_path_override(const std::string& v) {
    std::string t = trim_path(v);
    if (m_log_path_override == t) return;
    m_log_path_override = std::move(t);
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_custom_css_path_override(const std::string& v) {
    std::string t = trim_path(v);
    if (m_custom_css_path_override == t) return;
    m_custom_css_path_override = std::move(t);
    save();
    m_sig_changed.emit();
}

void AppPreferences::set_library_defaults_seeded(bool v) {
    if (m_library_defaults_seeded == v) return;
    m_library_defaults_seeded = v;
    save();
    m_sig_changed.emit();
}

// s152 — Toolbar density. 0..3 maps to Toolbar::Density enum values
// (Comfortable / Standard / Compact / Tight). Out-of-range values are
// clamped silently — callers passing through Toolbar::Density's enum
// values will always be in range; this clamp protects against
// inspector-driven raw int paths.
void AppPreferences::set_toolbar_density(int v) {
    if (v < 0) v = 0;
    if (v > 3) v = 3;
    if (m_toolbar_density == v) return;
    m_toolbar_density = v;
    save();
    m_sig_changed.emit();
}

// s202 m6 — quick-jump pick counters. See header for design.
//
// Returns 0 for any never-picked (phase, section) pair. The caller
// (quick-jump's sort) handles the zero-count case as the cold-start
// default ordering.
int AppPreferences::quick_jump_count(int phase,
                                      const std::string& section) const {
    auto it = m_quick_jump_counts.find({phase, section});
    if (it == m_quick_jump_counts.end()) return 0;
    return it->second;
}

// Increment the (phase, section) counter by one and persist. Called
// from the quick-jump's click handler before invoking focus_inspector_on.
// We persist on every bump because the map is tiny and the file is
// rewritten as a whole anyway; the cost is one disk write per Ctrl+Space
// pick, which is well under user-perceivable.
void AppPreferences::bump_quick_jump_count(int phase,
                                            const std::string& section) {
    if (section.empty()) return;
    auto& slot = m_quick_jump_counts[{phase, section}];
    if (slot < 1'000'000) ++slot;  // guard against pathological overflow
    save();
    m_sig_changed.emit();
}

} // namespace Curvz
