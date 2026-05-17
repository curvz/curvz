// scripting/path_is_safe.cpp ─────────────────────────────────────────────────
//
// s244 m1 — path containment for sensitive verbs. See path_is_safe.hpp
// for the contract and CANON.md "Path containment for sensitive verbs"
// for the design rationale.
//
// The implementation is small: cache two roots at startup, resolve
// the candidate path through realpath, prefix-check against the
// cached roots. Six lines of real work, padded out by the parent-
// directory fallback for not-yet-existing paths and the audit-friendly
// reason strings.

#include "scripting/path_is_safe.hpp"
#include "CurvzLog.hpp"

#include <cstdlib>     // getenv
#include <climits>     // PATH_MAX
#include <filesystem>  // path manipulation for the parent-walk
#include <string>

namespace curvz::scripting {

namespace {

// Cached roots, populated by init_roots(). Empty until init runs.
std::string g_home_root;
std::string g_tmp_root;
bool        g_initialised = false;

// Resolve through realpath. Returns empty string on failure.
//
// realpath() requires the target path to exist. Callers wrap this
// with the parent-walk fallback in resolve_or_walk_up() below.
std::string realpath_or_empty(const std::string& path) {
    if (path.empty()) return {};
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) == nullptr) {
        // ENOENT is the common case for save-target paths — the file
        // doesn't exist yet. Other errors (EACCES on parent dirs,
        // ELOOP for symlink cycles) all collapse to "couldn't
        // resolve"; the caller decides what to do next.
        return {};
    }
    return std::string(buf);
}

// Walk up the path's ancestors until realpath succeeds (or we run
// out of ancestors). Returns the resolved ancestor + the relative
// suffix that wasn't resolved, joined as a fully-resolved absolute
// path equivalent.
//
// Example: /home/scott/projects/new-thing/file.curvz where
// /home/scott/projects exists but /home/scott/projects/new-thing
// does not. We walk up to /home/scott/projects, realpath that to
// (say) /home/scott/projects (or wherever symlinks point), and
// re-attach /new-thing/file.curvz literally. The prefix check then
// applies to /home/scott/projects/new-thing/file.curvz, which is
// inside the realpath'd $HOME root iff /home/scott/projects is.
//
// This is the contract that lets `proj save_as <new-path>` work
// safely: the file doesn't exist yet but its parent does, and we
// check the parent.
std::string resolve_or_walk_up(const std::string& path) {
    // First shot — if the path exists, realpath gives us the answer.
    std::string direct = realpath_or_empty(path);
    if (!direct.empty()) return direct;

    // Path doesn't exist (probably). Walk up.
    std::filesystem::path p(path);
    std::filesystem::path suffix;

    while (p.has_parent_path() && p != p.parent_path()) {
        suffix = p.filename() / suffix;
        p = p.parent_path();

        std::string ancestor_resolved = realpath_or_empty(p.string());
        if (!ancestor_resolved.empty()) {
            // Re-attach the un-resolved suffix to the realpath'd
            // ancestor. The result is the path's "would-be" absolute
            // form post-creation, with the existing parts symlink-
            // resolved.
            std::filesystem::path joined(ancestor_resolved);
            joined /= suffix;
            return joined.string();
        }
    }

    // Nothing in the path's ancestry resolves. Either the path is
    // malformed or we don't have read access to the chain. Refuse
    // by returning empty.
    return {};
}

// True iff `path` (already realpath'd or walked-up) starts with
// `root` as a directory prefix. `root` is assumed to be a canonical
// directory path with no trailing slash. The check matches at a
// path-component boundary, not a string-substring boundary —
// "/home/scott" must not accept "/home/scottevil/file".
bool path_starts_with_root(const std::string& path,
                           const std::string& root) {
    if (root.empty()) return false;
    if (path.size() < root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    // Exact match (path == root) and proper-descendant
    // (path[root.size()] == '/') both count as "inside". A character
    // other than '/' immediately after the root prefix means a
    // different-named sibling directory ("/home/scott" vs
    // "/home/scottevil"); refuse those.
    if (path.size() == root.size()) return true;
    return path[root.size()] == '/';
}

// Strip trailing slashes from a path. Keeps a single leading slash
// for the root case ("/").
std::string strip_trailing_slash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

} // anon namespace

bool init_roots() {
    if (g_initialised) return true;

    const char* home_env = std::getenv("HOME");
    if (!home_env || *home_env == '\0') {
        LOG_WARN("path_is_safe: $HOME is unset; path containment "
                 "cannot initialise. All sensitive verbs will refuse "
                 "until $HOME is set and init_roots() is retried.");
        return false;
    }

    std::string home_resolved = realpath_or_empty(home_env);
    if (home_resolved.empty()) {
        LOG_WARN("path_is_safe: $HOME='{}' does not resolve through "
                 "realpath(); path containment cannot initialise.",
                 home_env);
        return false;
    }
    g_home_root = strip_trailing_slash(std::move(home_resolved));

    // $TMPDIR is allowed to be unset — fall back to /tmp. The
    // canonical Linux temp directory exists on every standard
    // install; if it doesn't, the install is broken in ways that
    // are not this utility's concern.
    const char* tmp_env = std::getenv("TMPDIR");
    std::string tmp_resolved;
    if (tmp_env && *tmp_env != '\0') {
        tmp_resolved = realpath_or_empty(tmp_env);
        if (tmp_resolved.empty()) {
            LOG_WARN("path_is_safe: $TMPDIR='{}' does not resolve; "
                     "falling back to /tmp.", tmp_env);
            tmp_resolved = realpath_or_empty("/tmp");
        }
    } else {
        tmp_resolved = realpath_or_empty("/tmp");
    }
    if (tmp_resolved.empty()) {
        // Last-ditch literal — /tmp must exist for the rule to make
        // sense; if even realpath("/tmp") fails, take the literal
        // string and let the prefix check do its job. Logs a warning
        // so the operator notices.
        LOG_WARN("path_is_safe: realpath('/tmp') failed; using "
                 "literal '/tmp' as the temp root. Symlinked "
                 "/tmp will not match.");
        tmp_resolved = "/tmp";
    }
    g_tmp_root = strip_trailing_slash(std::move(tmp_resolved));

    LOG_INFO("path_is_safe: roots initialised — HOME='{}' TMP='{}'",
             g_home_root, g_tmp_root);

    g_initialised = true;
    return true;
}

bool path_is_safe(const std::string& path, std::string* reason_out) {
    if (!g_initialised) {
        if (reason_out) *reason_out = "path-containment not initialised";
        LOG_WARN("path_is_safe('{}'): refused — init_roots() has not "
                 "been called or failed.", path);
        return false;
    }

    if (path.empty()) {
        if (reason_out) *reason_out = "empty path";
        return false;
    }

    std::string resolved = resolve_or_walk_up(path);
    if (resolved.empty()) {
        if (reason_out) *reason_out = "realpath failed (no resolvable ancestor)";
        LOG_WARN("path_is_safe('{}'): refused — no resolvable ancestor.",
                 path);
        return false;
    }

    if (path_starts_with_root(resolved, g_home_root)) return true;
    if (path_starts_with_root(resolved, g_tmp_root))  return true;

    if (reason_out) *reason_out = "outside $HOME and $TMPDIR";
    LOG_WARN("path_is_safe('{}'): refused — resolved='{}' is outside "
             "$HOME='{}' and $TMPDIR='{}'.",
             path, resolved, g_home_root, g_tmp_root);
    return false;
}

std::string_view home_root_for_testing() { return g_home_root; }
std::string_view tmp_root_for_testing()  { return g_tmp_root; }

} // namespace curvz::scripting
