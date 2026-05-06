#pragma once
#include <sigc++/signal.h>
#include <string>
#include <vector>

namespace Curvz {

// ══════════════════════════════════════════════════════════════════════════════
// RecentProjects — most-recently-used .curvz project folder paths (s144 m3)
//
// Singleton, mirrors the AppPreferences/MacroSystem pattern. Persists to
// ~/.config/curvz/recent_projects.json — separate from preferences.json
// because the recents list is high-churn list state, distinct in kind from
// the slow-changing app settings. Two concerns, two files.
//
// Storage shape on disk: { "paths": [ "/abs/path/to/foo.curvz", ... ] }.
// First entry is most-recent. Load is tolerant of missing file (first run),
// missing keys (defaults), and malformed entries (silently dropped).
//
// Lifecycle: load() at Application::on_activate (alongside AppPreferences).
// MainWindow::load_project() calls add() after every successful project
// load. The hamburger File ▸ Open Recent submenu is rebuilt on every
// signal_changed() emit — state-driven UI, no popover hooks needed.
//
// Pruning policy:
//   - On add():   trim to AppPreferences::recent_projects_max_count (default 10).
//   - On load():  silently drop entries whose path no longer exists on disk.
//                 Stale entries shouldn't survive across launches; rebuilding
//                 the menu would otherwise show ghost entries the user has
//                 to click before discovering the project moved.
//   - On clear(): list emptied, persisted, signal emitted.
//
// Path canonicalisation: stored as-is from the caller, except trimmed of
// trailing slashes so /foo/bar.curvz/ and /foo/bar.curvz are the same
// recent. Symlinks not resolved (deliberately — preserves the path the
// user opened by, which is what they'll recognise).
// ══════════════════════════════════════════════════════════════════════════════

class RecentProjects {
public:
    static RecentProjects& instance();

    // Load list from disk. Missing file → empty list. Missing entries dropped.
    // Safe to call multiple times (later loads replace the in-memory list).
    void load();

    // Persist to disk. Called automatically by add() / clear(); public for
    // explicit shutdown flush.
    void save() const;

    // The current list, most-recent first. Read-only; mutate via add()/clear().
    const std::vector<std::string>& paths() const { return m_paths; }

    // Insert path at front; if already present, move to front; trim to
    // max_count. Triggers save() and signal_changed(). No-op for empty input.
    void add(const std::string& path);

    // Drop a single entry by path (e.g. menu-time pruning of a missing file).
    // No-op if not present. Triggers save() and signal_changed() on hit.
    void remove(const std::string& path);

    // Empty the list and persist. Triggers signal_changed() unconditionally
    // (caller may want to refresh menu state even when list was already empty).
    void clear();

    // Emitted when the list changes via add / remove / clear / load.
    // Subscribers re-read paths(); the signal is parameterless to keep
    // ABI stable as call sites grow.
    sigc::signal<void()>& signal_changed() { return m_sig_changed; }

private:
    RecentProjects() = default;
    RecentProjects(const RecentProjects&) = delete;
    RecentProjects& operator=(const RecentProjects&) = delete;

    std::string recents_path() const;

    // Trim trailing slashes; return canonical key for membership tests.
    static std::string normalise(const std::string& path);

    std::vector<std::string> m_paths;
    sigc::signal<void()>     m_sig_changed;
};

} // namespace Curvz
