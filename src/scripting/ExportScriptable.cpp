// scripting/ExportScriptable.cpp ─────────────────────────────────────────────
//
// s251 m1 — `export` Scriptable. Second headless-verb singleton from
// ARC m5b (Tier 4 row ticks 1/~4-5 → 2/~4-5). First non-proj singleton
// in m5b; first format-bearing singleton (one Scriptable, multiple
// format-verbs as siblings).
//
// m1 ships ONE verb (`svg <path>`) plus two queries (`last_path`,
// `last_ok`). The svg writer (`SvgWriter::write_svg_file`) is the
// simplest of the five export formats — single path arg, no
// size/format/units parameters — and is the right shape for the
// singleton-opening milestone: validate the host's shape, the path-
// containment hookup, the query-as-observable pattern, all on the
// simplest format.
//
// Subsequent milestones (m2+) add the remaining four formats as
// sibling verbs on the same singleton:
//   - `png <path> <size>`       — size arg (longest-side pixels)
//   - `ico <path>`              — windows .ico bundle
//   - `refpt <path>`            — reference-points export (csv / json)
//   - `gresource <path>`        — full project bundle for resource compile
//
// All five share the same MainWindow-helper / path_is_safe pre-flight /
// last_path-last_ok observable shape; the per-format diff lives only in
// which writer the helper calls.
//
// See ExportScriptable.hpp for the verb / query surface, the per-verb
// refusal contract on `svg`, and the RunContext mask rationale
// (Scripter | TestRunner — same as proj save, not proj save_as).
//
// The body is small because the verb delegates to MainWindow's helper
// (`script_export_svg`), which packages the project-state checks
// (NoProject / NoActiveDoc) plus the call to SvgWriter::write_svg_file
// and returns an enum naming the outcome. The Scriptable maps the enum
// to either a happy-path `ok` (return Null + update last_path /
// last_ok) or a structured error throw (the listener catches and
// prints `error invoke threw: <message>`).
//
// path_is_safe pre-validates the path argument BEFORE calling the
// helper. Same split as save_as / load / new: path-safety in the
// Scriptable, state-check + I/O in MainWindow. The two gates stay in
// separate layers — the safety gate doesn't know about project state,
// the state-check gate doesn't know about path containment.
//
// The Scriptable / MainWindow split keeps Scriptable thin (DSL-facing
// string formatting + path-safety pre-flight + per-attempt observable
// state) and keeps MainWindow's private state inside MainWindow's API
// surface — no friendship, no public getters for m_project / m_canvas.

#include "scripting/ExportScriptable.hpp"

#include "CurvzLog.hpp"
#include "MainWindow.hpp"
#include "scripting/path_is_safe.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

namespace {

// Same coercion shape as ProjScriptable's anon-namespace helper. A
// bare token like /tmp/foo.svg arrives here as ValueKind::String
// (parse_literal's numeric parse fails on the path's slashes, falls
// through to text); a quoted "..." also arrives as ValueKind::String.
// Both paths land at the same return. Inlined per Scriptable rather
// than hoisted because the coercion is two lines and folding to a
// shared site would add a header dependency.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

} // anon namespace

ExportScriptable::ExportScriptable(Curvz::MainWindow* main_window)
    : Scriptable("export")
    , m_main_window(main_window) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "export". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime.
    //
    // m_last_path defaults to "" (default-constructed std::string);
    // m_last_ok defaults to false (in-class member initialiser in the
    // header). Initial state describes "no export has succeeded yet
    // this session" — same posture as ProjScriptable's `path` query
    // returning "" when no project is loaded.
}

ScriptValue ExportScriptable::invoke(std::string_view verb,
                                     const ScriptArgs& args) {
    LOG_INFO("DIAG ExportScriptable::invoke verb='{}' args.size={} mw={}",
             std::string(verb), args.size(),
             m_main_window ? "OK" : "NULL");
    if (!m_main_window) {
        // Defensive null check — see the ctor contract in the header:
        // nullptr would be a bug, but a hard crash here would be worse
        // than a structured refusal. Verb is interpolated so a png
        // null doesn't mis-blame svg when future format-verbs land.
        throw std::runtime_error(
            "export " + std::string(verb) + ": no main window");
    }

    if (verb == "svg") {
        // s251 m1 — first format-verb on this Scriptable.
        //
        // s251 m1 fix-re-ship — m_last_ok = false at the top of the
        // verb body, BEFORE any refusal branch can throw. The initial
        // ship spread the m_last_ok flip across the four "deeper"
        // refusal branches (path_is_safe + the three helper-result
        // refusals) but missed the two "shallow" argument-validation
        // branches at the top, because the throw at those branches
        // bypassed the per-branch flip discipline. The smoke caught
        // it: Phase 5's `assert export last_ok == false` after the
        // Phase 4 empty-path refusal FAILED with "expected false, got
        // true" — m_last_ok was still true from Phase 2's success.
        //
        // The structural fix: set m_last_ok = false ONCE at the top,
        // then flip to true ONLY on the Ok branch. The invariant
        // "any non-Ok outcome leaves m_last_ok false" now holds by
        // construction, not by per-branch discipline. Same posture
        // as "find the seam where the invariant lives, fix it there"
        // — the bug surfaced because the rule was procedural (six
        // sites that all had to remember the flip), and the fix
        // makes it structural (one site that sets the floor, one
        // site that lifts to ok).
        //
        // m_last_path is NOT touched here — it preserves the prior
        // success's path through any refusal. Only updated on the Ok
        // branch. See header design block on "last successful
        // export" semantics for the rationale.
        m_last_ok = false;

        // Argument validation: exactly one arg, coercible to string,
        // non-empty. Wrong shape is a structured throw — same posture
        // as proj save_as's wrong-arg refusal (script author needs to
        // see why nothing was written; silent-null would hide the
        // typo). m_last_ok already cleared above; throw is safe.
        if (args.size() != 1) {
            throw std::runtime_error(
                "export svg: expected exactly one path argument");
        }
        std::string path = arg_as_string(args[0]);
        if (path.empty()) {
            // Empty string OR wrong arg kind (arg_as_string returns
            // empty for non-String kinds). Either way the call is
            // unusable; same error message covers both because the
            // observable from the script author's perspective is
            // identical ("you didn't supply a usable path").
            //
            // path_is_safe would also refuse the empty string with
            // "empty path" — but the argument-validation guard
            // surfaces the same problem in the Scriptable's voice
            // rather than the path-containment utility's voice, which
            // is the right speaker for "you didn't even give me an
            // argument".
            throw std::runtime_error(
                "export svg: path argument is empty or not a string");
        }

        // Path-containment pre-flight. path_is_safe resolves the path
        // through realpath() (walking up to the nearest existing
        // ancestor for not-yet-existing target files — the common
        // export case where the output doesn't exist yet) and refuses
        // unless the resolved location is under $HOME or $TMPDIR. On
        // refusal, reason_out carries a one-line audit-friendly
        // string ("outside $HOME and $TMPDIR", "realpath failed (no
        // resolvable ancestor)", "empty path", "path-containment not
        // initialised"); we surface that string directly so the
        // script author can tell why their path was rejected.
        //
        // Fourth production call site of path_is_safe (after
        // proj save_as s247 m1, proj load s249 m1, proj new s250 m1).
        // First call site on a Scriptable OTHER than proj — the
        // utility's "one validator, many callers" design intent now
        // confirmed by a cross-Scriptable application.
        std::string reason;
        if (!path_is_safe(path, &reason)) {
            throw std::runtime_error(
                "export svg: " + reason);
        }

        // Path cleared the safety gate. Delegate to MainWindow's
        // helper for the project-state checks and the actual write.
        using R = Curvz::MainWindow::ScriptExportSvgResult;
        switch (m_main_window->script_export_svg(path)) {
            case R::Ok:
                // Happy path. Capture observables for the smoke's
                // assertable surface — m_last_path takes the just-
                // exported path; m_last_ok lifts to true (it was
                // cleared at the top of the verb body, so this is
                // the ONLY site that sets it true). Return Null so
                // the listener prints `ok`.
                m_last_path = path;
                m_last_ok   = true;
                return ScriptValue::null();
            case R::NoProject:
                throw std::runtime_error(
                    "export svg: no project loaded");
            case R::NoActiveDoc:
                throw std::runtime_error(
                    "export svg: project has no active document");
            case R::IoFailed:
                throw std::runtime_error(
                    "export svg: write failed");
        }
        // Unreachable — switch is exhaustive — but keeps the compiler
        // quiet about non-void return paths (mirrors save_as's tail).
        return ScriptValue::null();
    }

    // Unknown verb. Same posture as ProjScriptable's tail: the verb
    // dispatch fell through every known branch; surface a structured
    // error so a typo doesn't look like a silent success.
    throw std::runtime_error(
        "export: unknown verb '" + std::string(verb) + "'");
}

ScriptValue
ExportScriptable::query(std::string_view property) const {
    // Queries are not RunContext-gated (CANON's RunContext entry:
    // "Every model query. Reads can't corrupt."). Both queries
    // surface state local to this Scriptable — no MainWindow round-
    // trip needed.
    //
    // The "what was the last successful export" semantics:
    //   - last_path is the path of the LAST CALL THAT SUCCEEDED.
    //     Empty initially, never cleared by a failed call. A script
    //     that exports successfully then fails on a subsequent call
    //     sees last_path still pointing at the prior success.
    //   - last_ok is the outcome of the MOST RECENT CALL. true after
    //     a success, false after any failure, false initially.
    //
    // The split is deliberate: combining them ("last_path is empty
    // after any failure") would lose information; treating them as
    // independent surfaces the actual question each one answers.

    if (property == "last_path") {
        return ScriptValue::text(m_last_path);
    }
    if (property == "last_ok") {
        return ScriptValue::boolean(m_last_ok);
    }

    return ScriptValue::null();
}

std::vector<std::string> ExportScriptable::verbs() const {
    // s251 m1 — one verb (`svg`). m2+ adds png / ico / refpt /
    // gresource as the surface widens; the singleton's verb count
    // climbs from 1 to 5 across the arc.
    return {
        "svg",
    };
}

std::vector<std::string> ExportScriptable::properties() const {
    // s251 m1 — two queries.
    return {
        "last_path",
        "last_ok",
    };
}

// s251 m1 — RunContext mask declarations.
//
// `svg` — ctx::Scripter | ctx::TestRunner. Macro OUT. Same mask shape
//         as proj save (NOT proj save_as). Rationale: export produces
//         a side artefact at a path, it does not rewrite the project's
//         own identity. CANON's Scripter-in-scope list explicitly
//         permits "/tmp writes for scratch and diagnostic dumps" —
//         exporting an SVG to /tmp for visual inspection IS the
//         canonical use case. The path-containment gate ($HOME +
//         $TMPDIR) covers the system-integrity layer; Macro
//         exclusion covers the user-data layer's "recorded
//         automation should not silently write files" rule.
//
// Unknown verbs (typos, future verbs not yet declared) fall through
// to the base default of ctx::all_three. Matches the note in
// Scriptable::context_mask: "Unknown verb names (e.g. typos) SHOULD
// fall through to the default — the invoke() method itself is
// responsible for noticing the verb is unknown." The invoke()
// implementation above does noticed and throws on unknown verb, so
// the all_three fallthrough here is harmless.
//
// Seventh consumer of context_mask() (Inspector + proj.save +
// proj.save_as + proj.close + proj.load + proj.new + export.svg).
// Registry-promotion clock at 7/n and STILL HELD by design — see
// ExportScriptable.hpp's context_mask declaration block for the
// at-seven reasoning (svg's mask is sibling to proj.save's; the
// catalogue is becoming MORE uniform across seven verbs, not less,
// so the dedup argument that would drive registry promotion weakens
// rather than strengthens).
RunContextMask
ExportScriptable::context_mask(std::string_view verb) const {
    if (verb == "svg") {
        return ctx::Scripter | ctx::TestRunner;
    }
    return ctx::all_three;
}

} // namespace curvz::scripting
