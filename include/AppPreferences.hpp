#pragma once
#include <sigc++/signal.h>
#include <string>

namespace Curvz {

// ══════════════════════════════════════════════════════════════════════════════
// AppPreferences — application-tier user preferences (s139 m2)
//
// Persists across project switches and Curvz launches. Distinct from
// project-tier settings (which live in the .curvz file under canvas/document)
// and document-tier settings (which live on the SceneNode tree).
//
// Storage: ~/.config/curvz/preferences.json — flat JSON object. Schema is
// open for additions; load tolerates unknown keys and uses defaults for
// missing ones, so old install files keep working when new prefs land.
//
// Singleton-style access mirrors MacroSystem::instance(). One process,
// one preferences blob; load once at Application::on_activate, save on
// every change. No background sync, no file watcher — Curvz is the sole
// writer.
//
// Currently houses:
//   - boolean_cleanup_enabled — controls whether boolean op output goes
//     through the s139 keeper-set + cleanup-walk post-pass. Default false
//     during proof phase; flipped to true once the cleanup is trusted on
//     real workloads.
//
// Design intent: this file is a *first* preference, not a *last* one. The
// API shape (one getter + one setter per pref) scales cleanly. When the
// number of prefs grows past 4-5, consider a dedicated Settings inspector
// section that binds to the same backing store.
// ══════════════════════════════════════════════════════════════════════════════

class AppPreferences {
public:
    static AppPreferences& instance();

    // Load preferences from disk. Missing file → defaults. Malformed file →
    // logged warning and defaults. Safe to call multiple times.
    void load();

    // Persist to disk. Called automatically by setters; public for explicit
    // flush during shutdown if needed.
    void save() const;

    // ── Preferences ──────────────────────────────────────────────────────────
    bool boolean_cleanup_enabled() const { return m_boolean_cleanup_enabled; }
    void set_boolean_cleanup_enabled(bool v);

    // s139 m4: pass-2 triplet collapse on cleaned boolean output. Requires
    // boolean_cleanup_enabled to be on (Canvas::boolean_op gates this);
    // when set without cleanup, has no effect. Default off until the
    // user opts in via the menu.
    bool boolean_reduce_enabled() const { return m_boolean_reduce_enabled; }
    void set_boolean_reduce_enabled(bool v);

    // Emitted when any preference changes. Subscribers re-read the relevant
    // getter; the signal is intentionally parameterless to keep the ABI
    // stable as new prefs land.
    sigc::signal<void()>& signal_changed() { return m_sig_changed; }

private:
    AppPreferences() = default;
    AppPreferences(const AppPreferences&) = delete;
    AppPreferences& operator=(const AppPreferences&) = delete;

    std::string prefs_path() const;

    bool m_boolean_cleanup_enabled = false;
    bool m_boolean_reduce_enabled  = false;
    sigc::signal<void()> m_sig_changed;
};

} // namespace Curvz
