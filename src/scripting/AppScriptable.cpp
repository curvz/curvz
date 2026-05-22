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

AppScriptable::AppScriptable(Curvz::MainWindow* main_window,
                             EnactPenPath  enact_pen_path,
                             AnimateSvgFile animate_svg)
    : Scriptable("app")
    , m_main_window(main_window)
    , m_enact_pen_path(std::move(enact_pen_path))
    , m_animate_svg(std::move(animate_svg)) {
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

    // s288 m2 — `enact_pen_path "<d>" <speed>` verb. Kicks off a
    // welcome-demo Pen-path performance via the m_enact_pen_path
    // callback (MainWindow wires it to Canvas::welcome_enact_pen_path).
    //
    // Args (positional):
    //   0  d_string : SVG-d attribute string in user-space (Y-up,
    //                 bottom-left origin)
    //   1  speed    : double, multiplier on beat durations (1.0 nominal)
    //
    // Returns immediately. The animation runs asynchronously on the
    // main loop via Glib::Timeout owned by the WelcomeAnimator. The
    // script's next line dispatches before the animation completes.
    //
    // Gift-shape graceful degradation: missing args, empty d-string,
    // empty callback — all return ScriptValue::null() silently. No
    // exception, no error dialog. The audience sees no animation
    // rather than a broken state.
    //
    // This verb is m2's TESTING surface — the welcome demo's eventual
    // shape is a single `app.animate_svg` verb that orchestrates many
    // enact_pen_path calls (plus enact_color_pick, etc.) internally
    // from a parsed SVG. m2 exposes the lower-level verb so the smoke
    // can prove the Pen-path performance in isolation.
    if (verb == "enact_pen_path") {
        if (args.size() < 1) return ScriptValue::null();
        if (!m_enact_pen_path) return ScriptValue::null();

        std::string d_string;
        if (args[0].kind == ValueKind::String) d_string = args[0].s;
        if (d_string.empty()) return ScriptValue::null();

        double speed = 1.0;
        if (args.size() >= 2) {
            if      (args[1].kind == ValueKind::Double) speed = args[1].d;
            else if (args[1].kind == ValueKind::Int)
                speed = static_cast<double>(args[1].i);
        }
        if (speed <= 0.0) speed = 1.0;

        m_enact_pen_path(d_string, speed);
        return ScriptValue::null();
    }

    // s288 m3 — `animate_svg "<path>" <speed>` verb. SVG orchestrator
    // entry point. Same shape as enact_pen_path, different payload:
    // arg 0 is a filesystem path to an SVG file (m3 takes literal
    // filesystem paths; gresource-alias resolution is a future
    // enhancement).
    //
    // The animator parses the SVG via Curvz's own SvgParser, walks
    // the resulting temp doc tree, enqueues one Pen-path performance
    // per Path SceneNode, and plays them back-to-back with a small
    // inter-path breath. By orchestration end the doc has the SVG's
    // paths rendered as committed SceneNodes with their original
    // fill/stroke.
    //
    // Returns immediately. Same gift-shape graceful degradation as
    // enact_pen_path — empty path, missing file, parse failure, no
    // paths in the SVG: all silent no-ops.
    if (verb == "animate_svg") {
        if (args.size() < 1) return ScriptValue::null();
        if (!m_animate_svg) return ScriptValue::null();

        std::string svg_path;
        if (args[0].kind == ValueKind::String) svg_path = args[0].s;
        if (svg_path.empty()) return ScriptValue::null();

        double speed = 1.0;
        if (args.size() >= 2) {
            if      (args[1].kind == ValueKind::Double) speed = args[1].d;
            else if (args[1].kind == ValueKind::Int)
                speed = static_cast<double>(args[1].i);
        }
        if (speed <= 0.0) speed = 1.0;

        m_animate_svg(svg_path, speed);
        return ScriptValue::null();
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
    // s288 m2 — added the welcome-demo Pen-path performance verb.
    // s288 m3 — added the SVG orchestrator verb.
    return {
        "version",
        "gtk_version",
        "enact_pen_path",
        "animate_svg",
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
    // s288 m2 — welcome-demo Pen-path performance. Scripter for
    // interactive testing during dev (Scott runs it in the Scripter
    // window during the welcome demo arc); TestRunner for headless
    // smokes. Macro OUT: animation is wall-clock-dependent, replaying
    // a recorded macro would land at unpredictable times and the
    // recorded timing wouldn't be meaningful at replay.
    if (verb == "enact_pen_path") {
        return ctx::Scripter | ctx::TestRunner;
    }
    // s288 m3 — SVG orchestrator. Same rationale as enact_pen_path.
    if (verb == "animate_svg") {
        return ctx::Scripter | ctx::TestRunner;
    }
    return ctx::all_three;
}

} // namespace curvz::scripting
