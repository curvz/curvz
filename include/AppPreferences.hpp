#pragma once
#include <map>      // s202 m6: quick-jump counters
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
//   - reopen_last_project — bool, default true. When true, startup
//     auto-reopens the last-saved project path; when false, startup
//     falls through to a blank project. Read once at boot; no live
//     effect.
//   - recent_projects_max_count — int 1..50, default 10. Cap on the
//     recents list managed by RecentProjects (separate file). Surfaced
//     for users who want a longer or shorter Open Recent submenu.
//   - show_rulers_by_default — bool, default true. Boot-time ruler
//     visibility. The Ctrl+R toggle still flips per-session; this
//     pref controls the initial state on launch.
//   - undo_history_depth — int 50..10000, default 500. Maximum number
//     of undo steps. CommandHistory reads on every push, so reducing
//     the value trims the stack at the next operation.
//   - tooltip_delay_ms — int 0..2000, default 150. Delay before a
//     tooltip appears under a hovered widget. Applied to the GTK
//     "gtk-tooltip-timeout" property at startup; takes effect on next
//     launch. (Note: the property is deprecated in modern GTK and may
//     be a no-op on builds that have removed it — see Application.cpp.)
//   - library_path_override — string, default empty. Empty = use the
//     built-in default (~/.config/curvz/library). Non-empty = use this
//     directory for the user library tier. Takes effect on next launch.
//   - templates_path_override — string, default empty. Empty = use the
//     built-in default (~/.config/curvz/templates). Takes effect on
//     next launch.
//   - log_path_override — string, default empty. Empty = use the
//     built-in default (~/.local/share/curvz/curvz.log). Takes effect
//     on next launch. The override is consulted directly from JSON in
//     the Application constructor (before the AppPreferences singleton
//     loads) because spdlog needs a path before any log call can run.
//   - custom_css_path_override — string, default empty. Empty = use the
//     built-in default (~/.config/curvz/styles.css). Takes effect on
//     next launch.
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

    // s144 m2 — Reopen last project on startup.
    // Default true preserves the original boot path (auto-reopen the
    // last-saved project). When false, MainWindow::setup_project()
    // skips the load_last_project_path() check and falls through to
    // the blank-project branch. The pref is read once at startup;
    // changing it has no live effect, only takes hold on next launch.
    bool reopen_last_project() const { return m_reopen_last_project; }
    void set_reopen_last_project(bool v);

    // s144 m3 — Cap on the Open Recent submenu length, in entries.
    // Range 1..50; clamped on set. Default 10. RecentProjects::add()
    // re-reads this every insert, so changing the pref takes effect
    // immediately on the next project open (no relaunch needed).
    int  recent_projects_max_count() const { return m_recent_projects_max_count; }
    void set_recent_projects_max_count(int v);

    // s145 m1 — Show rulers by default on startup.
    // Default true preserves the historical boot state. MainWindow's
    // setup_menu reads this once to seed m_rulers_visible before the
    // toggle-rulers action is created; setup_layout then applies it
    // to the ruler widgets. Per-session Ctrl+R toggling is unaffected
    // — that flips the state in-memory without writing back to the
    // pref. The pref controls the initial state only.
    bool show_rulers_by_default() const { return m_show_rulers_by_default; }
    void set_show_rulers_by_default(bool v);

    // s145 m1 — Maximum number of undo steps.
    // Range 50..10000; clamped on set. Default 500.
    // CommandHistory::push reads this every operation, so reducing
    // the value trims the live stack at the next push (does not
    // retroactively prune). Increasing the value just allows the
    // stack to grow further from that point on.
    int  undo_history_depth() const { return m_undo_history_depth; }
    void set_undo_history_depth(int v);

    // s145 m1 — Tooltip delay in milliseconds.
    // Range 0..2000; clamped on set. Default 150 (Curvz historical
    // override of GTK's 500ms default — closer to Illustrator/Affinity
    // feel). Applied to the GTK "gtk-tooltip-timeout" property in
    // Application::on_activate. The property is deprecated in modern
    // GTK and dropped on some builds; the application probes for it
    // and silently no-ops if absent. Takes effect on next launch.
    int  tooltip_delay_ms() const { return m_tooltip_delay_ms; }
    void set_tooltip_delay_ms(int v);

    // s146 m3 — Warp creation defaults. When the user invokes Path ▸ Warp
    // on a selection, the resulting Warp's envelope is generated from
    // these settings rather than via a modal dialog. The inspector's
    // Application ▸ Warp subsection edits these values; the inspector's
    // Object ▸ Warp section binds to the live selected Warp's own state
    // (a separate concern). Both surfaces share the same controls, so
    // the values feel continuous: tweak defaults to taste, then Path ▸
    // Warp produces that shape; the just-created Warp is selected and
    // its live state is editable in the Object section.
    //
    //   warp_default_top_count  / warp_default_bot_count : 2..4 anchors
    //     per envelope. Default 2 (single-segment, the simplest shape).
    //   warp_default_preset : 0..7 index into curvz::utils::warp_presets
    //     names. Default 0 (Flat) — straight envelopes, the identity
    //     warp; user picks a preset deliberately when they want shape.
    //   warp_default_quality : 1..16. Default 4 (matches the historical
    //     dialog default).
    int  warp_default_top_count() const { return m_warp_default_top_count; }
    void set_warp_default_top_count(int v);
    int  warp_default_bot_count() const { return m_warp_default_bot_count; }
    void set_warp_default_bot_count(int v);
    int  warp_default_preset() const { return m_warp_default_preset; }
    void set_warp_default_preset(int v);
    int  warp_default_quality() const { return m_warp_default_quality; }
    void set_warp_default_quality(int v);

    // s145 m4 — Path overrides for built-in directories and files.
    // All four default to empty string; empty = "use the built-in
    // default" (each consumer falls through to its hardcoded default
    // when the override is empty). Non-empty = use this exact path.
    // Setters do no validation beyond trim; users may type a path
    // that doesn't exist yet, and consumers are expected to create
    // directories on demand the same way they do for the defaults.
    // All four take effect on next launch.
    const std::string& library_path_override() const { return m_library_path_override; }
    void set_library_path_override(const std::string& v);

    const std::string& templates_path_override() const { return m_templates_path_override; }
    void set_templates_path_override(const std::string& v);

    // s267 m1 — Override for the user-scripts directory consumed by the
    // Scripter window. Empty = default (~/.config/curvz/scripts). The
    // Scripter's folder-picker also writes here on accept, so a session
    // browse persists across launches. Takes effect on next launch (the
    // Scripter constructs once with the resolved path at MainWindow
    // ctor time; re-pointing mid-run is the picker's job, not a
    // preferences-write-triggered rescan).
    const std::string& scripts_path_override() const { return m_scripts_path_override; }
    void set_scripts_path_override(const std::string& v);

    // Read directly from preferences.json by Application constructor —
    // see Application.cpp::read_log_path_override_from_disk(). The
    // singleton getter is provided here for symmetry and inspector
    // round-tripping; the constructor cannot use it because the
    // singleton hasn't loaded yet at that point.
    const std::string& log_path_override() const { return m_log_path_override; }
    void set_log_path_override(const std::string& v);

    const std::string& custom_css_path_override() const { return m_custom_css_path_override; }
    void set_custom_css_path_override(const std::string& v);

    // s141: one-shot first-launch flag. LibraryPanel checks this on every
    // scan; if false, it creates the default category folders (arrows,
    // shapes, icons, ui) under the user library dir and flips this true.
    // Subsequent scans skip the seed pass, so a user who deletes a
    // default category via the context menu doesn't see it auto-recreated
    // on the next refresh.
    bool library_defaults_seeded() const { return m_library_defaults_seeded; }
    void set_library_defaults_seeded(bool v);

    // s152 — Toolbar density (Comfortable / Standard / Compact / Tight).
    // Stored as int 0..3 matching Toolbar::Density enum order. Default 1
    // (Standard) — the curvz-going-forward default after the s152 toolbar
    // refactor; clearly tighter than the historical 48-column Comfortable
    // and leaves headroom for ARC.md milestones #6-9 to add toolbar items
    // (Booleans, Step-and-Repeat, Blend, Warp).
    //
    // MainWindow reads this once at startup and calls Toolbar::set_density
    // with the corresponding enum. The user's right-click density picker
    // emits Toolbar::signal_density_changed(Density), which MainWindow
    // catches and writes back through set_toolbar_density().
    //
    // Range 0..3; clamped on set. Out-of-range values on load fall back
    // to the default (1 = Standard).
    int  toolbar_density() const { return m_toolbar_density; }
    void set_toolbar_density(int v);

    // s219 m1 — Scripter window enablement.
    // When true, Curvz boots with the Scripter window presented and the
    // headerbar "monkey" toggle visible; the Developer ▸ Scripting menu
    // item and the Application ▸ Developer inspector switch are
    // checked. When false (default), the Scripter window still exists
    // (always-constructed at startup, single app-owned instance), but
    // it stays hidden and the monkey button is removed from the
    // headerbar. Toggling the pref live presents/hides the window and
    // updates the headerbar without restart.
    //
    // The pref is read by MainWindow on construction to seed the action
    // state, the headerbar button visibility, and the inspector switch.
    // signal_changed() drives the live response on later toggles.
    //
    // Stored as `scripter_enabled` in preferences.json; default false so
    // users opening Curvz for the first time after the s219 m1 rollout
    // get the production-looking app without the scripting surface
    // intruding.
    bool scripter_enabled() const { return m_scripter_enabled; }
    void set_scripter_enabled(bool v);

    // s202 m6 — Quick-jump counter store. Tracks how often the user
    // picks each (phase, section) pair from the Ctrl+Space quick-jump.
    // The float orders candidates by descending count, so the user's
    // actual habits surface to the top over time. Zero-count entries
    // sink; new sections start at zero and float up as they're used.
    //
    // Phase is a small enum:
    //   0 = Setup     — no active doc OR doc has no content.
    //   1 = Execution — selection is non-empty.
    //   2 = Polish    — content exists but no selection.
    //
    // Phase detection is the caller's job (MainWindow computes it at
    // quick-jump invocation time); this surface only stores +
    // serializes. JSON key form: "<phase_int>:<section_title>".
    //
    // The map is small (typically 3 phases × ~14 sections = ~42
    // entries). No decay, no MRU — raw counts. If a section falls out
    // of use the user simply stops picking it; if their habits change
    // the counts catch up.
    int  quick_jump_count(int phase, const std::string& section) const;
    void bump_quick_jump_count(int phase, const std::string& section);

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
    // s144 m2 — default true preserves the historical auto-reopen
    // behaviour for users with an existing preferences.json that
    // doesn't yet have this key. Users opt out via the inspector.
    bool m_reopen_last_project = true;
    // s144 m3 — cap on Open Recent submenu length. 10 matches Photoshop /
    // Illustrator / VS Code defaults; range 1..50 per set_recent_projects_max_count.
    int  m_recent_projects_max_count = 10;
    // s145 m1 — boot-time ruler visibility. Default true preserves the
    // historical "rulers visible on launch" behaviour.
    bool m_show_rulers_by_default = true;
    // s145 m1 — undo history depth. 500 was the historical hardcoded
    // CommandHistory::MAX_HISTORY value. Range 50..10000 per setter.
    int  m_undo_history_depth = 500;
    // s145 m1 — tooltip delay in ms. 150 was the s85 cont-6 hardcoded
    // override of GTK's 500ms default. Range 0..2000 per setter.
    int  m_tooltip_delay_ms = 150;
    // s146 m3 — Warp creation defaults. See public docs above.
    int  m_warp_default_top_count = 3;
    int  m_warp_default_bot_count = 3;
    int  m_warp_default_preset    = 0;   // Flat
    int  m_warp_default_quality   = 4;
    // s145 m4 — path overrides. Empty = "use the built-in default"
    // (each consumer falls through to its own default). Non-empty
    // = use this exact path. Stored as plain strings; no path-shape
    // validation here.
    std::string m_library_path_override;
    std::string m_templates_path_override;
    std::string m_log_path_override;
    std::string m_custom_css_path_override;
    // s267 m1 — override for the Scripter's user-scripts directory.
    // Empty falls through to ~/.config/curvz/scripts at the consumer
    // (see scripts_user_dir() in MainWindow.cpp). Picker writes here.
    std::string m_scripts_path_override;
    bool m_library_defaults_seeded = false;
    // s152 — Toolbar density. 0=Comfortable, 1=Standard (default),
    // 2=Compact, 3=Tight. Maps to Toolbar::Density enum.
    int  m_toolbar_density = 1;

    // s219 m1 — Scripter enablement. Default false: scripting surface
    // stays hidden until the user opts in via Developer ▸ Scripting
    // (hamburger menu) or Application ▸ Developer (inspector).
    bool m_scripter_enabled = false;

    // s202 m6 — quick-jump (phase, section) → pick count. Persisted
    // as a flat object in preferences.json under "quick_jump_counts"
    // with keys of the form "<phase>:<title>". Empty by default;
    // counts grow as the user picks sections from the quick-jump.
    std::map<std::pair<int, std::string>, int> m_quick_jump_counts;

    sigc::signal<void()> m_sig_changed;
};

} // namespace Curvz
