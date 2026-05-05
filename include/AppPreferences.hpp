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
//   - boolean_cleanup_quality — int 0..10. Controls the boolean-op cleanup
//     post-pass (s142 m6: keeper-set + apex pinning + span filler). The
//     slider exposes the algorithm's two tunables (apex_min_turn_deg,
//     max_untagged_run) as one user-facing knob, plus a "raw" position
//     at the high end:
//       0  = most aggressive cleanup (most node reduction)
//       5  = balanced (s142 m6 default)
//       10 = no cleanup — raw Clipper2 output (most faithful to Clipper2,
//            hundreds of nodes, no algorithm intervention)
//     Higher = more faithful to Clipper2 = more nodes preserved.
//     Migrated from the old boolean_cleanup_enabled bool: true→5, false→10
//     (the bool's "false" meant "use raw Clipper2," which is q=10's role).
//   - library_defaults_seeded — internal one-shot first-launch flag,
//     not user-facing.
//
// Design intent: this file is the home for application-tier user
// preferences. The s143 inspector "Application" group surfaces the
// user-facing prefs here as a top-level inspector category alongside
// Project / Document / Object. Future additions land as new sections
// under that group: recent-projects max count, autosave debounce,
// log file path, custom CSS path. Each new pref: one getter + one
// setter + one row in build_app_section.
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
    // Boolean cleanup quality (s143 m1) — replaces the s139 m2
    // boolean_cleanup_enabled bool.  Range 0..10:
    //   0   → most aggressive cleanup  (apex=30°, max_run=20)
    //   5   → default                  (apex=15°, max_run=10  — s142 m6 anchor)
    //   10  → no cleanup, raw Clipper2 polyline output (most faithful to
    //         Clipper2; algorithm is bypassed entirely)
    //
    // Higher = more nodes preserved. The slider goes left-to-right from
    // "Approximate (fewer nodes)" to "Faithful (more nodes)" — and the
    // most-faithful end is no algorithm at all. The mapping itself
    // lives in Canvas (single source of truth at the point of use); this
    // layer just stores the int.
    int  boolean_cleanup_quality() const { return m_boolean_cleanup_quality; }
    void set_boolean_cleanup_quality(int v);

    // s141: one-shot first-launch flag. LibraryPanel checks this on every
    // scan; if false, it creates the default category folders (arrows,
    // shapes, icons, ui) under the user library dir and flips this true.
    // Subsequent scans skip the seed pass, so a user who deletes a
    // default category via the context menu doesn't see it auto-recreated
    // on the next refresh.
    bool library_defaults_seeded() const { return m_library_defaults_seeded; }
    void set_library_defaults_seeded(bool v);

    // Emitted when any preference changes. Subscribers re-read the relevant
    // getter; the signal is intentionally parameterless to keep the ABI
    // stable as new prefs land.
    sigc::signal<void()>& signal_changed() { return m_sig_changed; }

private:
    AppPreferences() = default;
    AppPreferences(const AppPreferences&) = delete;
    AppPreferences& operator=(const AppPreferences&) = delete;

    std::string prefs_path() const;

    // s143 m1 — quality slider replaces the s139 m2 enabled toggle.
    // Default 5 = the s142 m6 hardcoded values (apex=15°, max_run=10).
    int  m_boolean_cleanup_quality = 5;
    bool m_library_defaults_seeded = false;
    sigc::signal<void()> m_sig_changed;
};

} // namespace Curvz
