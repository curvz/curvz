// scripting/InspectorScriptable.cpp ──────────────────────────────────────────
//
// s222 m2 — thin Scriptable wrapper over MainWindow's inspector-area
// section orchestration methods. See InspectorScriptable.hpp for the
// verb surface and the design rationale for why this is its own
// Scriptable (not a verb on pnl_styles, not a PropertiesPanel-only
// surface).
//
// The whole TU is small because both verbs delegate directly to
// existing MainWindow methods:
//
//   open "<title>"          → MainWindow::apply_section_open(title, true)
//   collapse_all            → MainWindow::collapse_all_inspector_sections()
//
// No project getter, no command pushing, no cross-registry plumbing —
// MainWindow already owns the cross-registry orchestration. The
// Scriptable's only job is to receive verb dispatches and call the
// right method.

#include "scripting/InspectorScriptable.hpp"
#include "scripting/Scriptable.hpp"
#include "scripting/path_is_safe.hpp"   // s244 m2 — diagnostic surface for m5c

#include "CurvzLog.hpp"   // DIAG
#include "MainWindow.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

namespace {

// Same shape as the argument coercion helpers in the model
// Scriptables. Inlined here rather than hoisted because the
// coercion is two lines and folding to a shared site would add a
// header dependency.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

} // anon namespace

InspectorScriptable::InspectorScriptable(Curvz::MainWindow* main_window)
    : Scriptable("inspector")
    , m_main_window(main_window) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "inspector". MainWindow holds us as a member; the
    // registry entry lives for the window's lifetime.
}

ScriptValue InspectorScriptable::invoke(std::string_view verb,
                                        const ScriptArgs& args) {
    LOG_INFO("DIAG InspectorScriptable::invoke verb='{}' args.size={} mw={}",
             std::string(verb), args.size(),
             m_main_window ? "OK" : "NULL");
    if (!m_main_window) return ScriptValue::null();

    if (verb == "open") {
        // Open a single named section without collapsing siblings.
        // Empty / wrong-kind / unknown-title args degrade as no-ops
        // (MainWindow::apply_section_open silently skips unknown
        // titles; we mirror that posture on bad args too).
        if (args.empty()) return ScriptValue::null();
        std::string title = arg_as_string(args[0]);
        LOG_INFO("DIAG   open: title='{}' arg.kind={}", title,
                 static_cast<int>(args[0].kind));
        if (title.empty()) return ScriptValue::null();
        m_main_window->apply_section_open(title, /*on=*/true);
        return ScriptValue::null();
    }

    if (verb == "collapse_all") {
        // No args. Close every section across both registries
        // (m_sec_apply + PropertiesPanel::m_section_open). The
        // implementation lives in MainWindow_helpers.cpp; see the
        // s202 m6 design block in MainWindow.hpp for the rationale
        // behind spanning both registries.
        LOG_INFO("DIAG   collapse_all dispatching");
        m_main_window->collapse_all_inspector_sections();
        return ScriptValue::null();
    }

    // ── s244 m2 diagnostic verbs (m5c smoke surface) ────────────────────────
    //
    // None of these mutate state. They live on `inspector` because it
    // is the natural meta-host; they exercise the m5c structural rules
    // (path_is_safe + RunContext gating) without needing a sensitive
    // verb on disk. See the "Diagnostic surface" section in the header
    // for the full design rationale.
    //
    // path_is_safe is exercised through QUERIES (home_root_is_safe,
    // tmp_root_is_safe, etc_is_safe, slash_is_safe, empty_is_safe) in
    // query() rather than as a verb taking a path arg. The reason is
    // assertability: the DSL's `assert` form is
    //   `assert <object> <property> == <literal>`
    // which means only QUERY results are assertable. A verb that takes
    // a path arg and returns Boolean is observable in the trace but
    // not assertable. The per-case query shape sacrifices flexibility
    // (the caller can't supply arbitrary paths) for assertability
    // (the smoke can assert each case's expected outcome), which is
    // the right trade for the smoke's purposes.

    if (verb == "diagnose_universal") {
        // Accept-path observable for the smoke. Universal mask
        // (inherited default — context_mask() doesn't override for
        // this verb). Always succeeds; always returns "ok".
        return ScriptValue::text("ok");
    }

    if (verb == "diagnose_test_runner_only") {
        // Refuse-path observable for the smoke. context_mask()
        // declares this verb as TestRunner-only; from any caller
        // other than TestRunner, the dispatcher's gating check
        // refuses BEFORE this body is reached. So in practice the
        // smoke (running in Scripter context) never sees this
        // return value — it sees the dispatcher's structured
        // refusal error instead. The body is still implemented
        // (returning "ok") for completeness so a future CLI test
        // runner that legitimately wires TestRunner context will
        // see a clean success.
        return ScriptValue::text("ok");
    }

    return ScriptValue::null();
}

ScriptValue InspectorScriptable::query(std::string_view property) const {
    // s244 m2 diagnostic queries — surface the path_is_safe cached
    // roots, their initialised state, and per-case path-safety
    // observables. Reads only; no mutation, no RunContext gating
    // (queries are not gated — see CANON's RunContext entry,
    // "Every model query. Reads can't corrupt.").
    //
    // The per-case path-safety queries (home_root_is_safe etc.)
    // each call path_is_safe() with a hardcoded path and return the
    // Boolean. This shape lets the smoke `assert` the result
    // directly — the DSL's assert form is equality-on-query-result,
    // so the verb-with-arg path can be observed but not asserted.
    // See the design comment in invoke() for the trade-off.

    if (property == "home_root") {
        // String view → owned string for the ScriptValue. Empty if
        // init_roots() hasn't run or failed.
        return ScriptValue::text(
            std::string(curvz::scripting::home_root_for_testing()));
    }
    if (property == "tmp_root") {
        return ScriptValue::text(
            std::string(curvz::scripting::tmp_root_for_testing()));
    }
    if (property == "home_root_initialised") {
        return ScriptValue::boolean(
            !curvz::scripting::home_root_for_testing().empty());
    }
    if (property == "tmp_root_initialised") {
        return ScriptValue::boolean(
            !curvz::scripting::tmp_root_for_testing().empty());
    }

    // ── Path-safety per-case observables ────────────────────────────────
    //
    // Each query calls path_is_safe() once with a fixed path. The
    // smoke asserts the expected Boolean for each case, covering the
    // acceptance side (home_root and tmp_root are by construction
    // safe) and the refusal side (/etc/passwd is outside both roots;
    // / is shallower than either root by construction; empty string
    // is the degenerate case path_is_safe refuses).

    if (property == "home_root_is_safe") {
        // The cached home_root IS the $HOME root — it must be safe
        // by construction (it equals one of the two valid prefixes).
        std::string root(curvz::scripting::home_root_for_testing());
        return ScriptValue::boolean(
            curvz::scripting::path_is_safe(root, nullptr));
    }
    if (property == "tmp_root_is_safe") {
        // Same shape for the cached tmp_root.
        std::string root(curvz::scripting::tmp_root_for_testing());
        return ScriptValue::boolean(
            curvz::scripting::path_is_safe(root, nullptr));
    }
    if (property == "etc_is_safe") {
        // /etc is universally outside $HOME and /tmp by Linux FHS
        // convention. Refusal case for the prefix check.
        return ScriptValue::boolean(
            curvz::scripting::path_is_safe("/etc/passwd", nullptr));
    }
    if (property == "slash_is_safe") {
        // Filesystem root. Strictly shallower than either cached
        // root (both are deeper paths by construction), so the
        // prefix check refuses.
        return ScriptValue::boolean(
            curvz::scripting::path_is_safe("/", nullptr));
    }
    if (property == "empty_is_safe") {
        // Empty path — path_is_safe rejects with "empty path" reason
        // (degenerate case handled at the top of the predicate).
        return ScriptValue::boolean(
            curvz::scripting::path_is_safe("", nullptr));
    }

    // s222 m2 — no queries on this Scriptable beyond the s244 m2
    // diagnostic surface above. See the "Out of scope" block in the
    // header for the deferred section-introspection query surface
    // (section_titles, is_open, etc.).
    return ScriptValue::null();
}

std::vector<std::string> InspectorScriptable::verbs() const {
    return {
        "open",
        "collapse_all",
        // s244 m2 — diagnostic surface for m5c. The path-safety
        // per-case observables live as queries (in properties()
        // below) for assertability; the only verb-side observables
        // here are the RunContext gating accept/refuse pair.
        "diagnose_universal",
        "diagnose_test_runner_only",
    };
}

std::vector<std::string> InspectorScriptable::properties() const {
    // s244 m2 — diagnostic state queries for m5c. See header comment
    // for the rationale on why these live on inspector rather than a
    // dedicated diagnostics Scriptable. The path-safety per-case
    // queries (*_is_safe) wrap path_is_safe() with hardcoded paths
    // so the smoke can assert the expected Boolean per case.
    return {
        "home_root",
        "tmp_root",
        "home_root_initialised",
        "tmp_root_initialised",
        "home_root_is_safe",
        "tmp_root_is_safe",
        "etc_is_safe",
        "slash_is_safe",
        "empty_is_safe",
    };
}

// s244 m2 — RunContext mask declarations. Default (inherited from
// Scriptable base) is ctx::all_three for every verb; we override
// here ONLY to mark diagnose_test_runner_only as TestRunner-only.
// Every other verb on this Scriptable falls through to the base
// default. See CANON's "RunContext gates the verb surface" entry
// for the declaration discipline.
RunContextMask
InspectorScriptable::context_mask(std::string_view verb) const {
    if (verb == "diagnose_test_runner_only") {
        return ctx::TestRunner;
    }
    return ctx::all_three;
}

} // namespace curvz::scripting
