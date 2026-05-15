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

    return ScriptValue::null();
}

ScriptValue InspectorScriptable::query(std::string_view /*property*/) const {
    // No queries in m2. See the "Out of scope" block in the header
    // for the deferred query surface (section_titles, is_open, etc.).
    return ScriptValue::null();
}

std::vector<std::string> InspectorScriptable::verbs() const {
    return {
        "open",
        "collapse_all",
    };
}

std::vector<std::string> InspectorScriptable::properties() const {
    return {};  // see query() note
}

} // namespace curvz::scripting
