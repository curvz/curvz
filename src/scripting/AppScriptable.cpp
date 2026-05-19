// scripting/AppScriptable.cpp ────────────────────────────────────────────────
//
// s263 m2 — THIRD headless-verb singleton implementation. Opens the
// `app` Scriptable at the code level. One verb (`version`) ships in
// m2 mirroring the proj.path / proj.has_path read template; m3 adds
// `gtk_version` as the second pure-read verb on the same singleton.
//
// s263 m3 — `gtk_version` added (no row tick; verb-on-existing-
// singleton). Both verbs are pure-read; same trust profile; same
// shape. The two ship in one delivery because they share structural
// shape — see header for the "discipline-transfer SIXTH instance"
// note.
//
// s263 v2 fix — both reads promoted to DUAL-SURFACE (verb + property)
// after smoke 63's first run showed `assert app version == "0.9.0"`
// failing with `expected "0.9.0", got null`. The DSL's `assert` is
// equality-only on query results (same constraint smoke 44's proj-
// save comment block names verbatim); the v1 ship took the verb-only
// path on a wrong reading of how the assertion routes. v2 adds
// matching query() branches and properties() entries — both reads are
// now reachable BOTH ways: imperative `app version` returns the value
// via invoke; `assert app version == "..."` reaches the same value via
// query. Convergent-evidence rule held: the v1 verb echo printed the
// right string (`= "0.9.0"`); the assertion's null was the gap. Same
// shape as proj.path / proj.has_path — pure-read identity exposed as
// property so assertions can reach it.
//
// ── Why this file is small ─────────────────────────────────────────────────
//
// Unlike ProjScriptable.cpp (~660 lines) and ExportScriptable.cpp
// (~700 lines), this file is tiny — both verbs return a const string
// with no refusal branches, no path-safety pre-flight, no project-
// state interrogation, no command-history threading. The shape isn't
// a sign of underbuilt-ness; it's a sign that "report process
// identity" is genuinely a one-liner once the build-time and runtime
// hooks are in place. The CMakeLists block that defines CURVZ_VERSION
// IS the work for `version`; this file just dereferences it.

#include "scripting/AppScriptable.hpp"

#include "CurvzLog.hpp"

#include <gtk/gtk.h>  // gtk_get_major_version() / _minor_ / _micro_ for m3

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// CURVZ_VERSION is plumbed via target_compile_definitions in
// CMakeLists.txt from project(curvz VERSION X.Y.Z). The macro is
// always defined by a normal build of the project; this fallback
// exists for two reasons:
//
//   1. Defensive — if a future CMakeLists edit accidentally drops
//      the target_compile_definitions line, the file still compiles
//      and the version verb returns "unknown" rather than failing
//      to build. The smoke would catch the regression
//      (assert app version == "0.9.0" would FAIL, observable in the
//      trace), so the defensive fallback doesn't hide bugs — it
//      shifts the failure from compile-time-error to test-time-
//      failure, which is the right shape (test failure is louder
//      and more diagnostic than a missing-macro compile error
//      thirty lines deep in a preprocessor expansion).
//
//   2. Sandbox-safe — if anyone ever builds this file outside the
//      project CMake (e.g. a single-file fuzz harness or a doc-
//      generation tool), the file still compiles. Same shape as
//      the GTK4 LOG_INFO macros' defensive #ifndef guards.
#ifndef CURVZ_VERSION
#define CURVZ_VERSION "unknown"
#endif

namespace curvz::scripting {

AppScriptable::AppScriptable(Curvz::MainWindow* main_window)
    : Scriptable("app")
    , m_main_window(main_window) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "app". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime. The MainWindow pointer
    // is stashed but unused in m2 / m3 — see header for the rationale
    // (kept for symmetry with proj / export and for future verbs).
}

ScriptValue AppScriptable::invoke(std::string_view verb,
                                  const ScriptArgs& args) {
    LOG_INFO("DIAG AppScriptable::invoke verb='{}' args.size={} mw={}",
             std::string(verb), args.size(),
             m_main_window ? "OK" : "NULL");

    // s263 m2 — `version` verb. Pure read; no MainWindow state access.
    // Zero-arg only — any positional argument is a misuse of the verb
    // and refuses with a structured error mirroring the export
    // Scriptable's argument-count guards (Branch B equivalent).
    if (verb == "version") {
        if (!args.empty()) {
            throw std::runtime_error(
                "app version: expected zero arguments");
        }
        // CURVZ_VERSION is a string-literal macro plumbed from
        // CMakeLists via target_compile_definitions. The expansion
        // happens at preprocess time; the runtime cost is one
        // pointer copy into the ScriptValue.
        return ScriptValue::text(CURVZ_VERSION);
    }

    // s263 m3 — `gtk_version` verb. Pure read; reports runtime GTK
    // library version rather than the build-against version. Same
    // zero-arg shape as `version`.
    if (verb == "gtk_version") {
        if (!args.empty()) {
            throw std::runtime_error(
                "app gtk_version: expected zero arguments");
        }
        // gtk_get_major_version() and friends are guaranteed to
        // return values for the currently-loaded GTK; they're
        // initialised by GTK's own startup before main() reaches
        // here. std::to_string is the simplest stringify; the dotted
        // format matches conventional version-string presentation
        // (e.g. "4.14.5"), which is what bug reports want and what
        // the smoke can substring-assert against once the listener
        // gains a starts-with primitive (m4+ work).
        std::string s = std::to_string(gtk_get_major_version());
        s += '.';
        s += std::to_string(gtk_get_minor_version());
        s += '.';
        s += std::to_string(gtk_get_micro_version());
        return ScriptValue::text(s);
    }

    // Unknown verb falls through to a structured error. Same shape
    // as the unknown-verb branches in proj / export Scriptables.
    throw std::runtime_error(
        "app: unknown verb '" + std::string(verb) + "'");
}

ScriptValue AppScriptable::query(std::string_view property) const {
    // s263 v2 — both reads are dual-surface (verb + property). The
    // property path exists so the DSL's equality-only `assert` can
    // reach the same value the verb path returns when invoked
    // imperatively. See header "Query surface" block for the
    // rationale and the s263 v1 → v2 trace evidence.
    //
    // Queries are NOT RunContext-gated — reads can't corrupt
    // (CANON's RunContext entry: "Every model query. Reads can't
    // corrupt."). Both branches mirror the invoke-side bodies
    // exactly; the duplication is two lines and consolidating into
    // a helper would obscure that the contracts are independent.

    if (property == "version") {
        return ScriptValue::text(CURVZ_VERSION);
    }

    if (property == "gtk_version") {
        std::string s = std::to_string(gtk_get_major_version());
        s += '.';
        s += std::to_string(gtk_get_minor_version());
        s += '.';
        s += std::to_string(gtk_get_micro_version());
        return ScriptValue::text(s);
    }

    // Unknown property — listener handles the Null fall-through as
    // "unknown property" rather than a hard error, matching every
    // Scriptable's behaviour for unrecognised property names.
    return ScriptValue::null();
}

std::vector<std::string> AppScriptable::verbs() const {
    // s263 m2 / s263 m3 — two pure-read verbs.
    return {
        "version",
        "gtk_version",
    };
}

std::vector<std::string> AppScriptable::properties() const {
    // s263 v2 — dual-surface posture: both reads are verbs AND
    // properties. See header "Query surface" block.
    return {
        "version",
        "gtk_version",
    };
}

// s263 m2 / s263 m3 — RunContext mask declarations.
//
// Both verbs declare ctx::Scripter | ctx::TestRunner. Macro is OUT
// for both — recorded-macro replay on a different build (for
// `version`) or a different system (for `gtk_version`) means the
// recorded value is stale at replay time, which violates the
// recorded-macro mental model. The mask shape is identical to
// proj.save and export.svg; the catalogue uniformises further at
// the s263 close.
//
// Unknown verbs (typos, future verbs not yet declared) fall through
// to the base default of ctx::all_three. That matches the note in
// Scriptable::context_mask: "Unknown verb names (e.g. typos) SHOULD
// fall through to the default — the invoke() method itself is
// responsible for noticing the verb is unknown." (Invoke's
// unknown-verb branch above handles the actual refusal.)
//
// Tenth and eleventh consumers of context_mask() (Inspector +
// 5 proj verbs + 2 export verbs + 2 app verbs). Registry-promotion
// clock at 10/n and STILL HELD by design — see header
// AppScriptable.hpp's context_mask declaration block for the
// at-ten reasoning (sibling shape to proj.save / export.svg; no
// novel mask shape; catalogue uniformising further).
RunContextMask
AppScriptable::context_mask(std::string_view verb) const {
    if (verb == "version") {
        return ctx::Scripter | ctx::TestRunner;
    }
    if (verb == "gtk_version") {
        return ctx::Scripter | ctx::TestRunner;
    }
    return ctx::all_three;
}

} // namespace curvz::scripting
