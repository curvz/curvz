// scripting/path_is_safe.hpp ─────────────────────────────────────────────────
//
// s244 m1 — first instance of ARC m5c (security architecture, Tier 5).
//
// The structural rule: any verb that takes a path argument resolves the
// path through `realpath()` first, then refuses the call if the resolved
// path is not under `$HOME` or `$TMPDIR` (typically `/tmp`). Two roots,
// computed at process start, baked in. One utility, every singleton
// calls it.
//
// See CANON.md "Path containment for sensitive verbs — `$HOME` and
// `/tmp`" for the design rationale, threat model, and the catalogue
// of what this rule does and does not protect against.
//
// ── Why this lives in `curvz::scripting`, not `curvz::utils` ──────────
//
// The path-containment surface is specifically about *which paths a
// scripted caller can pass to a sensitive verb*. It is not a general
// file-system utility — Curvz's GUI Save / Save As / Open / Export
// flows do not call it (the user picked the path through a native file
// picker; the OS-level permissions and the user's explicit gesture
// are the trust signals there). The function only has callers in the
// scripting layer — m5b's headless-verb singletons (`proj save_as`,
// `export png`, etc.). Living in `curvz::scripting` keeps the
// boundary visible: any time something includes this header, it's
// shipping a sensitive verb.
//
// ── Why `realpath` before the prefix check ────────────────────────────
//
// A script that writes to `/tmp/innocent.txt` is fine. A script that
// writes to `/tmp/innocent.txt` where that file is a symlink to
// `/etc/passwd` is the exact thing the rule has to refuse. Symlink
// resolution before the prefix check is non-negotiable; otherwise the
// rule has a one-line bypass.
//
// `realpath()` requires the path to exist on the filesystem for
// successful resolution. For verbs that *create* a file (the common
// case: `proj save_as <new-path>`), the existence check fails. The
// implementation handles this by resolving the *parent directory* of
// the requested path and applying the prefix check there — if the
// directory the file would land in is safe, the file's containment
// is guaranteed.
//
// ── Why the two roots are computed at process start ───────────────────
//
// `$HOME` can change mid-process if the script (or Curvz itself) calls
// `setenv("HOME", ...)`. Caching the resolved roots at process start
// makes the safety check deterministic: the process commits to a fixed
// home and a fixed temp directory at launch, and stays committed.
//
// `init_roots()` must be called exactly once, early in process
// startup (Application::on_startup is the natural site). Subsequent
// calls are a no-op — the cached roots are immutable for the
// process's lifetime. `path_is_safe` calls before `init_roots` return
// false with a structured reason ("path-containment not initialised");
// that's a programmer error, not a runtime data issue.
//
// ── Why a `reason_out` parameter, not exceptions ──────────────────────
//
// Sensitive verbs need to surface a refusal as a script-level error
// (visible in the trace, audit-logged). Exceptions would force every
// caller to wrap the check in try/catch. The `reason_out` parameter
// lets the caller pass through a structured rejection string to the
// dispatcher's error path with no control-flow gymnastics.
//
// ── What this does NOT do ──────────────────────────────────────────────
//
// - Does not protect the user's *own* files inside `$HOME`. That's
//   the RunContext layer's job — see CANON.md "RunContext gates the
//   verb surface" (shipping in s244 m2).
// - Does not check whether the caller has write permission. The OS
//   does that; `path_is_safe` returning true means "the path is in a
//   permitted region," not "the caller can definitely write here."
// - Does not concern itself with file extension or content type.
//   `proj save_as` validates its own extension separately; this
//   utility only knows about the path's location in the filesystem.

#pragma once
#include <string>
#include <string_view>

namespace curvz::scripting {

// One-time initialisation. Resolves `$HOME` and `$TMPDIR` through
// `realpath()` and caches the absolute, symlink-resolved roots for
// the process's lifetime. Call once at startup before any sensitive
// verb dispatches; subsequent calls are silently no-op.
//
// Falls back to `/tmp` if `$TMPDIR` is unset. Returns false (and logs
// a warning) if `$HOME` is unset or doesn't resolve — in that case
// `path_is_safe` will refuse every path until init is retried. Most
// real environments resolve cleanly; the failure mode is reserved
// for headless CI containers with a malformed env.
bool init_roots();

// True iff `path` resolves through `realpath()` to a location under
// the cached `$HOME` or `$TMPDIR` roots. Symlinks are resolved before
// the prefix check.
//
// For a path that doesn't yet exist (the file-creation case), the
// resolution attempt walks up to the nearest existing ancestor and
// applies the prefix check there. The intent: a verb that would
// create a new file in a safe directory is safe; a verb that would
// create a new file in a directory that itself is unsafe is unsafe.
//
// If `reason_out` is non-null, on refusal it receives a one-line
// human-readable reason ("outside $HOME and /tmp", "realpath
// failed", "path-containment not initialised"). On acceptance it
// is left untouched. The string is intended for the dispatcher's
// audit log and the script-level error trace; it is not localised
// and not part of any API contract.
bool path_is_safe(const std::string& path,
                  std::string* reason_out = nullptr);

// Testing/diagnostic accessors. The roots are private to the .cpp
// after init_roots() runs; these expose them read-only so a future
// smoke can assert "HOME root is $HOME, TMPDIR root is /tmp" without
// reaching back into the env. Empty strings before init_roots() is
// called.
std::string_view home_root_for_testing();
std::string_view tmp_root_for_testing();

} // namespace curvz::scripting
