// scripting/AppScriptable.hpp ────────────────────────────────────────────────
//
// s263 m2 — THIRD headless-verb singleton from ARC m5b. Opens after
// `proj` (s246 m1) and `export` (s251 m1). Ticks Tier 4 row 2 → 3.
//
// `app` is the application-level singleton — one registry entry per
// app session, no per-instance proxy. m2 ships ONE verb (`version`,
// pure read) and m3 adds the second pure-read verb (`gtk_version`).
// The verb surface is intentionally tiny to validate the singleton
// shape; future milestones may add `quit` (bucket-C destructive,
// TestRunner-only mask) or further read verbs as use cases name them.
//
// See CANON.md "Headless-verb singletons" for the design rule this
// implements. Unlike `proj` (which routes File-menu items past the
// OS picker) and `export` (which routes per-format writers past the
// export dialog), `app` exists to surface PROCESS-SCOPE IDENTITY
// — the build version baked at compile-time and the GTK library
// version linked at runtime. The "leapfrog the picker" rationale
// doesn't apply; `app` has no picker to leapfrog. The rationale is
// observability: scripts that produce bug-report-grade traces (the
// dominant TestRunner use case) need to capture which Curvz built
// against which GTK they're testing, and the only honest way to
// answer that is to ask the running process.
//
// ── Why this is a singleton (not a per-anything Scriptable) ────────────────
//
// The application is a process-scope concept — there's exactly one
// per Curvz invocation. The "what version is this" outcome doesn't
// address any document, any project, any user-data object — it
// addresses the running BINARY AS A WHOLE. Same singleton-host
// argument as proj and export: per-instance proxies are wrong
// shape (cardinality 1), naming the application as a special doc is
// wrong shape (it isn't a document).
//
// Sibling to ProjScriptable and ExportScriptable in the shape sense
// — flat verb surface, no per-instance proxies, MainWindow-pointer
// constructor, held by MainWindow as a unique_ptr member. The
// MainWindow pointer is unused in m2 / m3 (both verbs read process-
// scope constants rather than MainWindow state) but kept in the ctor
// signature for symmetry with proj / export, and because future
// verbs (`app.quit`, `app.main_window_title`, etc.) will need it.
//
// ── Addressing ─────────────────────────────────────────────────────────────
//
//   app                     — the Scriptable. ONE registry entry per
//                             app session; held as a member by
//                             MainWindow, registered for the lifetime
//                             of the window. No per-instance proxy.
//
// `can_resolve` / `proxy_for` are unimplemented (inherit base no-op).
//
// ── Verb surface (s263 m2: `version`; s263 m3: + `gtk_version`) ────────────
//
//   version                 — read the build version string baked at
//                             compile time. Returns the value of the
//                             CURVZ_VERSION macro, plumbed via
//                             target_compile_definitions in CMakeLists
//                             from project(curvz VERSION X.Y.Z). The
//                             version string flows CMake → C++ → DSL
//                             with no hand-maintained mirror, so
//                             future CMake version bumps automatically
//                             surface here. Zero arguments; returns
//                             ScriptValue::text. No refusal branches
//                             (read can't fail meaningfully — the
//                             macro is always defined by the build,
//                             see the CMakeLists block where
//                             target_compile_definitions is added).
//
//                             Trust profile: Scripter | TestRunner.
//                             Same mask as proj.save and export.svg.
//                             Macro is OUT — reading the version inside
//                             a recorded macro produces output that
//                             becomes meaningful only at replay-time
//                             on a possibly-different build, which
//                             violates the recorded-macro mental model
//                             (recorded macros replay user actions,
//                             not snapshot process state). Pure-read
//                             verbs aren't dispatcher-gated for query
//                             form, but invoke-form IS gated; this
//                             verb is invoke-form (it produces a
//                             return value, not a property snapshot),
//                             so the mask applies.
//
//                             Contract — one happy path, no refusals:
//
//                             - Always returns the build version string.
//                               Extra arguments are rejected by the
//                               argument-count guard at the top of
//                               invoke(). No "no project" branch (the
//                               verb doesn't address project state).
//                               No "drag in flight" branch (the verb
//                               doesn't touch canvas).
//
//   gtk_version             — read the gtkmm RUNTIME version. Returns
//                             ScriptValue::text formatted as
//                             "<major>.<minor>.<micro>", e.g. "4.14.5",
//                             via gtk_get_major_version() / minor /
//                             micro (C-level functions, available
//                             through gtkmm's transitive include of
//                             gtk/gtk.h). Distinct from `version` in
//                             a way that matters: `version` reports
//                             BUILD-TIME Curvz identity (baked at
//                             compile, doesn't change between user
//                             runs of the same binary); `gtk_version`
//                             reports RUNTIME GTK identity (the
//                             library actually loaded by the dynamic
//                             linker at process start, which can
//                             differ from the GTK Curvz was built
//                             against on a system where the user
//                             updated GTK without rebuilding Curvz).
//                             For bug reports, both numbers are
//                             needed to characterise the environment.
//
//                             Trust profile: Scripter | TestRunner.
//                             Same rationale as `version` — Macro
//                             OUT because recorded-macro replay on
//                             a different system would mean the
//                             recorded value is stale at replay time.
//
//                             Contract — one happy path, no refusals.
//                             Same shape as `version`.
//
//   animating_parser_smoke "<svg-path>"
//                          — s290 m1a. Diagnostic verb. Parses the
//                            given SVG via both the base parser and
//                            the new AnimatingSvgParser, walks both
//                            resulting CurvzDocuments, and logs a
//                            one-line "shape signature" (counts of
//                            major node types) for each. Equal
//                            signatures on real SVGs = m1a passes
//                            (AnimatingSvgParser is producing
//                            identical output to the base parser).
//
//                            Self-contained — no MainWindow / Canvas
//                            plumbing required. Result is observed
//                            via curvz.log, not via return value;
//                            the verb returns ScriptValue::null().
//
//                            Trust profile: Scripter | TestRunner.
//                            Macro OUT (log-channel diagnostic).
//
//   animating_emitter_smoke "<svg-path>"
//                          — s290 m1b.1 / m1b.2 / m1b.3. Diagnostic
//                            verb. Parses the given SVG via
//                            AnimatingSvgParser wired to a
//                            LoggingEmitter consumer, then cross-
//                            checks the emit stream against the
//                            doc-shape walk's counts.
//
//                            m1b.1 gate: on_path count == paths.
//                            m1b.2 gates: compound depth returns to
//                            zero; compound_begin count == compounds;
//                            group depth returns to zero;
//                            group_begin count == groups; begin/end
//                            pairs balanced.
//                            m1b.3 gates: layer depth returns to
//                            zero; layer_begin count == layers
//                            (Type::Layer only — technical layer
//                            kinds skip events); layer begin/end
//                            balanced; doc_metadata fires exactly
//                            once.
//
//                            MATCH = every gate passed. DIFFER lists
//                            which gate failed so the bug is bisected
//                            to one event family.
//
//                            Self-contained; result via curvz.log;
//                            same trust profile as the m1a verb.
//
// ── Why no `quit` verb yet ────────────────────────────────────────────────
//
// `app.quit` is the natural sibling to `proj.close` (both end an
// active surface; both would gate TestRunner-only by the surface-
// preservation rule). It's deliberately NOT shipped in m2 / m3 — pure-
// read openers have been the historical pattern at singleton-open
// time (proj opened with `save` AND read queries; export opened with
// `svg` and read queries). `app.quit` adds a destructive verb to a
// singleton that currently has zero footing in the corpus; the
// minimum-viable-surface rule says open with reads, accumulate verbs
// in subsequent milestones. m3+ work can add `quit` once the
// singleton has been exercised in smoke and the cross-verb
// interactions (e.g. quit-during-active-script-trace) have a story.
//
// ── Query surface ─────────────────────────────────────────────────────────
//
// Both reads (`version`, `gtk_version`) are addressable as BOTH verbs
// AND properties. The verb path returns the value when invoked
// imperatively (`app version` in the Scripter); the property path
// lets the DSL's `assert` reach the same value via the query-result
// equality form (`assert app version == "0.9.0"`).
//
// The s263 v1 ship took the verb-only path on the theory that
// `app version` could funnel through the same evaluation either way.
// Smoke 63's first run surfaced the gap: the DSL's `assert` is
// equality-only on query results (see ScriptListener / Scripter
// header notes), so `assert app version == ...` reaches query() not
// invoke(); m2's empty properties() vector caused the listener to
// return Null, FAIL reported as `expected "0.9.0", got null`. Same
// shape ProjScriptable's `path` / `has_path` use — they're queries,
// not verbs, precisely so smoke 44's preconditions can assert against
// them. The dual-surface posture aligns this Scriptable with the
// established pattern: pure-read identity is exposed as both a verb
// (for imperative use from a script) and a property (for assertions
// in test traces). Future invoke-form verbs (`app.quit`, etc.) stay
// verb-only because they have side effects and no meaningful "read"
// shape.
//
// Both methods compute the same value the same way — the property
// branch in query() and the verb branch in invoke() each independently
// dereference CURVZ_VERSION or call gtk_get_*_version(). The duplication
// is two lines; consolidating into a private helper would obscure that
// each method's contract is independent (queries aren't dispatcher-
// gated; invokes are). Keep the two paths visually parallel.
//
// ── Trust profile summary ─────────────────────────────────────────────────
//
// Both verbs declare ctx::Scripter | ctx::TestRunner. Macro OUT for
// both per the per-verb rationale above. Tenth and eleventh consumers
// of context_mask() (Inspector + 5 proj + 2 export + 2 app);
// registry-promotion clock is at 10/n. **STILL HELD by design** —
// the new instances are sibling shape to proj.save / export.svg
// (the same Scripter | TestRunner constant applied to fourth and
// fifth verb-name strings within m5b). No novel mask shape; catalogue
// is uniformising further. See ProjScriptable.hpp's context_mask
// declaration block for the trigger-condition discipline; s263 doesn't
// change the call.
//
// ── Lifetime ──────────────────────────────────────────────────────────────
//
// Held by MainWindow as a unique_ptr member. Constructed in
// MainWindow's ctor (after `proj` and `export`); destroyed in
// MainWindow's dtor. The MainWindow pointer passed at construction
// is non-owning; the Scriptable's entire lifetime is contained in
// MainWindow's. Same lifetime story as ProjScriptable /
// ExportScriptable — out-of-line dtor in MainWindow.cpp so the
// unique_ptr at the header can hold an incomplete type.

#pragma once

#include "Scriptable.hpp"
#include "ScriptValue.hpp"
#include "RunContext.hpp"

#include <functional>   // s288 m2 — EnactPenPath callback type
#include <string>
#include <string_view>
#include <vector>

namespace Curvz {
class MainWindow;
}

namespace curvz::scripting {

class AppScriptable : public Scriptable {
public:
    // s288 m2 — welcome-demo Pen-path performance callback. The
    // enact_pen_path verb routes through this; MainWindow wires it to
    // a lambda that calls Canvas::welcome_enact_pen_path. May be empty;
    // the verb is a silent no-op in that case (gift-shape graceful
    // degradation — the audience sees nothing rather than an error).
    using EnactPenPath = std::function<void(const std::string& d_string,
                                            double speed)>;

    // s288 m3 — welcome-demo SVG orchestrator callback. The animate_svg
    // verb routes through this; MainWindow wires it to a lambda that
    // calls Canvas::welcome_animate_svg_file. Same shape as EnactPenPath
    // — May be empty (silent no-op).
    using AnimateSvgFile = std::function<void(const std::string& svg_path,
                                              double speed)>;

    // Registers as "app" via the Scriptable base ctor.
    // `main_window` is non-owning; the Scriptable is held as a member
    // of MainWindow and destroyed in MainWindow's dtor, so the pointer
    // is valid for the Scriptable's entire lifetime. Unused in m2 / m3
    // (both verbs read process-scope state, not MainWindow state) but
    // kept in the ctor signature for symmetry with proj / export and
    // for future verbs that will need it.
    //
    // s288 m2 — second ctor arg: EnactPenPath callback for the
    // welcome-demo arc's first beat verb.
    // s288 m3 — third ctor arg: AnimateSvgFile callback for the
    // SVG-orchestrator verb.
    explicit AppScriptable(Curvz::MainWindow* main_window,
                           EnactPenPath  enact_pen_path  = {},
                           AnimateSvgFile animate_svg    = {});
    ~AppScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // s263 m2 / s263 m3 — RunContext mask declarations.
    //
    //   version       — ctx::Scripter | ctx::TestRunner. Macro OUT
    //                   per the per-verb rationale in the header
    //                   block above (recorded macro replaying a
    //                   version read on a different build means the
    //                   recorded value is stale at replay time;
    //                   violates the recorded-macro mental model).
    //   gtk_version   — ctx::Scripter | ctx::TestRunner. Same
    //                   rationale as version — Macro OUT because
    //                   replay on a different system means stale
    //                   recorded value.
    //
    // Tenth and eleventh consumers of context_mask() (Inspector +
    // 5 proj verbs + 2 export verbs + 2 app verbs). Registry-
    // promotion clock at 10/n, STILL HELD by design. See header
    // block "Trust profile summary" for the at-ten reasoning.
    RunContextMask context_mask(std::string_view verb) const override;

private:
    Curvz::MainWindow* m_main_window;
    EnactPenPath       m_enact_pen_path;  // s288 m2 — welcome-demo
                                          // Pen-path performance hook.
                                          // Routes to Canvas via
                                          // MainWindow lambda. May be
                                          // empty (silent no-op).
    AnimateSvgFile     m_animate_svg;     // s288 m3 — SVG-orchestrator
                                          // hook. Routes to Canvas via
                                          // MainWindow lambda. May be
                                          // empty (silent no-op).
};

} // namespace curvz::scripting
