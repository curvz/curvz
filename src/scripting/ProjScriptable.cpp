// scripting/ProjScriptable.cpp ───────────────────────────────────────────────
//
// s246 m1 — first headless-verb singleton from ARC m5b. Wraps the
// project-level save outcome.
//
// s247 m1 — second verb: `save_as <path>`. First path-argument verb on
// this Scriptable; first live exercise of curvz::scripting::path_is_safe
// under user-supplied input. Third consumer of context_mask() — the
// registry-promotion clock reaches 3/3 and is HELD by design.
//
// s248 m1 — third verb: `close`. No path argument. Refuses on the
// same three structural state errors save does (NoProject /
// Dragging / Dirty), where Dirty is the conversion of
// on_close_project's check_unsaved_then modal into a structured
// refusal (CANON's headless-verb-singletons rule: scripts can't
// summon modals). Also adds the `dirty` query — exposes the same
// undo-stack proxy the GUI's check_unsaved_then uses. Fourth
// consumer of context_mask(); promotion STILL held.
//
// **s248 m1 fix re-ship — close mask narrowed.** The initial s248
// m1 ship declared close as ctx::Scripter | ctx::TestRunner by
// analogy to save's mask shape. Testing surfaced a segfault when
// the smoke called proj close from Scripter context: the teardown
// invalidates state the Scripter window depends on, and subsequent
// script lines crash. The deeper rule: scripts can't safely
// destroy their own RESULT SURFACE — even patching the segfault,
// a successful close leaves the script's trace unobservable
// because the project the trace refers to is gone. close is now
// ctx::TestRunner only, sibling to save_as. See the verb-surface
// block at `close` in the header for the surface-preservation
// rule (the structural sibling to path containment).
//
// s249 m1 — fourth verb: `load <path>`. First destructive-
// replacement verb. Same code shape as save_as (one path arg,
// path_is_safe pre-flight, MainWindow helper); same TestRunner-
// only mask as save_as and close. Second verb under the
// surface-preservation rule. Notably, the helper does NOT need a
// do_load_project lift — MainWindow::load_project() already IS
// the orchestrator the GUI's on_open delegates to, so the
// pump-at-the-seam already exists at the helper-orchestrator
// boundary. Fifth consumer of context_mask(); promotion STILL
// held (same-mask-as-existing-verb argument applies again).
//
// s250 m1 — fifth verb: `new <path>`. Closes the m5b proj-surface
// arc at 5 of 5 File-menu items (save / save_as / close / load /
// new). Sibling to load in code shape (same one-path-arg +
// path_is_safe + MainWindow helper pattern; same no-do_-lift
// because load_project IS the orchestrator the GUI delegates to);
// sibling to load in posture (TestRunner-only by the surface-
// preservation rule when replacing an existing project; same
// stricter-than-GUI Dirty refusal). Mechanically distinct: load
// reads existing disk, new creates fresh disk via
// CurvzProject::create_empty. The construction difference
// introduces one new structural refusal — TargetExists — that
// load doesn't have. Sixth consumer of context_mask(); promotion
// STILL held (fourth instance of the same TestRunner-only mask
// within this Scriptable; no novel shape).
//
// (Subsequent milestones: `force_close`, `force_load`, `force_new`
// when use cases name the need — all TestRunner-only by the same
// surface-preservation rule.)
//
// See ProjScriptable.hpp for the verb / query surface, the per-verb
// refusal contracts, and the design rationale for why this is a
// singleton (no per-document Scriptable, no proxy routing).
//
// The body is small because each verb delegates to MainWindow's
// script-side helper (`script_save_project` / `script_save_project_as`
// / `script_close_project` / `script_load_project` /
// `script_new_project`), which packages the same state checks the
// GUI handler does plus the call to the underlying writer / teardown
// / factory method. The helper returns an enum naming the outcome;
// the Scriptable maps that enum to either a happy-path `ok` (return
// Null) or a structured error throw (the listener catches and prints
// `error invoke threw: <message>`).
//
// save_as / load / new additionally pre-validate the path argument
// through path_is_safe BEFORE calling the helper. That split (path-
// safety in the Scriptable, state-check + I/O in MainWindow) keeps
// the two gates in separate layers — the safety gate doesn't know
// about project state, the state-check gate doesn't know about path
// containment.
//
// The Scriptable / MainWindow split keeps Scriptable thin (DSL-facing
// string formatting + path-safety pre-flight on save_as) and keeps
// MainWindow's private state inside MainWindow's API surface — no
// friendship, no public getters for m_project / m_canvas / m_history.

#include "scripting/ProjScriptable.hpp"

#include "CurvzLog.hpp"
#include "MainWindow.hpp"
#include "scripting/path_is_safe.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

namespace {

// Same shape as the argument coercion helper in InspectorScriptable
// (and the model Scriptables). Inlined per Scriptable rather than
// hoisted because the coercion is two lines and folding to a shared
// site would add a header dependency. A bare token like
// /tmp/foo.curvz arrives here as ValueKind::String (parse_literal's
// numeric parse fails on the path's slashes, falls through to text);
// a quoted "..." also arrives as ValueKind::String. Both paths land
// at the same return.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

} // anon namespace

ProjScriptable::ProjScriptable(Curvz::MainWindow* main_window)
    : Scriptable("proj")
    , m_main_window(main_window) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "proj". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime.
}

ScriptValue ProjScriptable::invoke(std::string_view verb,
                                   const ScriptArgs& args) {
    LOG_INFO("DIAG ProjScriptable::invoke verb='{}' args.size={} mw={}",
             std::string(verb), args.size(),
             m_main_window ? "OK" : "NULL");
    if (!m_main_window) {
        // Defensive null check — see the ctor contract in the header:
        // nullptr would be a bug, but a hard crash here would be worse
        // than a structured refusal. Verb is interpolated so a
        // save_as null doesn't mis-blame save.
        throw std::runtime_error(
            "proj " + std::string(verb) + ": no main window");
    }

    if (verb == "save") {
        // Delegate to MainWindow's helper. The helper packages the
        // same state checks the GUI on_save handler does (project,
        // path, drag) plus the actual save call; we just map its
        // return-enum to the Scriptable's outward contract.
        using R = Curvz::MainWindow::ScriptSaveResult;
        switch (m_main_window->script_save_project()) {
            case R::Ok:
                // The helper handled update_title() + LOG_INFO on
                // success. Returning Null causes the listener to
                // print `ok`.
                return ScriptValue::null();
            case R::NoProject:
                throw std::runtime_error("proj save: no project loaded");
            case R::NoPath:
                // The "would-fall-through-to-Save-As-picker" branch.
                // CANON's headless-verb-singletons rule says leapfrog
                // the picker; save's argument is implicit in the
                // project's directory state, so the absence of that
                // state IS the refusal trigger. Point to the future
                // save_as verb (s247).
                throw std::runtime_error(
                    "proj save: no saved path; use "
                    "'proj save_as <path>' first (s247)");
            case R::Dragging:
                // The GUI handler defers via 100ms signal_timeout in
                // this case (mid-drag saves corrupt node data). A
                // Scriptable verb that returned `ok` synchronously
                // while a deferred save fired later would lie. Refuse
                // and let the script re-issue cleanly.
                throw std::runtime_error(
                    "proj save: canvas drag in progress");
            case R::IoFailed:
                // CurvzProject::save() returned false. Mirrors the
                // GUI handler's LOG_ERROR path.
                throw std::runtime_error("proj save: save failed");
        }
        // Unreachable — switch is exhaustive — but keeps the compiler
        // quiet about non-void return paths.
        return ScriptValue::null();
    }

    if (verb == "save_as") {
        // s247 m1 — first path-argument verb on this Scriptable.
        //
        // Argument validation: exactly one arg, coercible to string,
        // non-empty. Wrong shape is a structured throw — same posture
        // as save's refusal branches (script author needs to see why
        // nothing was written; silent-null would hide the typo).
        if (args.size() != 1) {
            throw std::runtime_error(
                "proj save_as: expected exactly one path argument");
        }
        std::string path = arg_as_string(args[0]);
        if (path.empty()) {
            // Empty string OR wrong arg kind (arg_as_string returns
            // empty for non-String kinds). Either way the call is
            // unusable; same error message covers both because the
            // observable from the script author's perspective is
            // identical ("you didn't supply a usable path").
            throw std::runtime_error(
                "proj save_as: path argument is empty or not a string");
        }

        // Path-containment pre-flight. path_is_safe resolves the path
        // through realpath() (walking up to the nearest existing
        // ancestor for not-yet-existing target files — the common
        // save_as case) and refuses unless the resolved location is
        // under $HOME or $TMPDIR. On refusal, reason_out carries a
        // one-line audit-friendly string ("outside $HOME and $TMPDIR",
        // "realpath failed (no resolvable ancestor)", "empty path",
        // "path-containment not initialised"); we surface that string
        // directly so the script author can tell why their path was
        // rejected.
        //
        // NOTE: from Scripter context this branch is structurally
        // unreachable because the dispatcher's RunContext gate
        // refuses save_as BEFORE invoke() runs (save_as's mask is
        // ctx::TestRunner only; Scripter is not in it). The branch
        // is wired live for the future TestRunner caller — the
        // verb's safety architecture is complete today, even though
        // today's only caller can't exercise it through this path.
        std::string reason;
        if (!path_is_safe(path, &reason)) {
            throw std::runtime_error(
                "proj save_as: " + reason);
        }

        // Path cleared the safety gate. Delegate to MainWindow's
        // helper for the project-state checks and the actual write.
        using R = Curvz::MainWindow::ScriptSaveAsResult;
        switch (m_main_window->script_save_project_as(path)) {
            case R::Ok:
                return ScriptValue::null();
            case R::NoProject:
                throw std::runtime_error(
                    "proj save_as: no project loaded");
            case R::Dragging:
                throw std::runtime_error(
                    "proj save_as: canvas drag in progress");
            case R::IoFailed:
                throw std::runtime_error(
                    "proj save_as: save failed");
        }
        // Unreachable — switch is exhaustive — but keeps the compiler
        // quiet about non-void return paths (mirrors save's tail).
        return ScriptValue::null();
    }

    if (verb == "close") {
        // s248 m1 — third verb on this Scriptable. No arguments.
        // Reject calls with extras so a typo like `proj close foo`
        // isn't silently accepted (the listener might otherwise
        // tokenise `foo` and pass it through ignored).
        if (!args.empty()) {
            throw std::runtime_error(
                "proj close: takes no arguments");
        }

        // Delegate to MainWindow's helper. The helper runs the same
        // structural checks on_close_project's check_unsaved_then
        // does (project loaded, drag, dirty) plus the standard drag
        // check, then delegates teardown to do_close_project. We
        // just map the return-enum to the Scriptable's outward
        // contract.
        using R = Curvz::MainWindow::ScriptCloseResult;
        switch (m_main_window->script_close_project()) {
            case R::Ok:
                // The helper handled the teardown + LOG_INFO on
                // success. Returning Null causes the listener to
                // print `ok`.
                return ScriptValue::null();
            case R::NoProject:
                throw std::runtime_error(
                    "proj close: no project loaded");
            case R::Dragging:
                throw std::runtime_error(
                    "proj close: canvas drag in progress");
            case R::Dirty:
                // The GUI prompts Save/Discard/Cancel here via
                // check_unsaved_then; scripts can't summon modals.
                // Refuse and point the script author at the explicit
                // save (or save_as for never-saved projects). A
                // future force_close verb (s250+) will give the
                // discard path; for now, refusing rather than
                // silently dropping work is the safer default.
                throw std::runtime_error(
                    "proj close: project has unsaved changes; "
                    "save first ('proj save' or 'proj save_as "
                    "<path>') or wait for a future force_close");
        }
        // Unreachable — switch is exhaustive — but keeps the
        // compiler quiet about non-void return paths (mirrors save's
        // and save_as's tails).
        return ScriptValue::null();
    }

    if (verb == "load") {
        // s249 m1 — fourth verb on this Scriptable. First
        // destructive-replacement verb. Same code shape as save_as
        // (one path arg, path_is_safe pre-flight, MainWindow helper);
        // same TestRunner-only mask as save_as and close.
        //
        // Argument validation mirrors save_as exactly: exactly one
        // arg, coercible to string, non-empty. Wrong shape is a
        // structured throw (same posture as every refusal branch on
        // this Scriptable — script author needs to see why nothing
        // was loaded; silent-null would hide the typo).
        if (args.size() != 1) {
            throw std::runtime_error(
                "proj load: expected exactly one path argument");
        }
        std::string path = arg_as_string(args[0]);
        if (path.empty()) {
            // Empty string OR wrong arg kind (arg_as_string returns
            // empty for non-String kinds). Same combined message as
            // save_as; the observable from the script author's
            // perspective is identical.
            throw std::runtime_error(
                "proj load: path argument is empty or not a string");
        }

        // Path-containment pre-flight. path_is_safe resolves the path
        // through realpath() and refuses unless the resolved location
        // is under $HOME or $TMPDIR. Second production call site for
        // path_is_safe (save_as was the first); same Scripter-
        // unreachability note applies — the dispatcher's RunContext
        // gate refuses load BEFORE invoke() runs (load's mask is
        // ctx::TestRunner only; Scripter is not in it). The branch is
        // wired live for the future TestRunner caller.
        //
        // Subtle difference from save_as: load reads an EXISTING
        // .curvz directory, where save_as writes a NEW one. For
        // an existing path, path_is_safe's realpath() resolves
        // directly (no walk-up-to-nearest-existing-ancestor needed
        // because the target already exists). The accept/refuse
        // matrix is the same; the behaviour-under-the-hood is just
        // simpler in the common case.
        std::string reason;
        if (!path_is_safe(path, &reason)) {
            throw std::runtime_error(
                "proj load: " + reason);
        }

        // Path cleared the safety gate. Delegate to MainWindow's
        // helper. The helper does drag check + dirty check +
        // CurvzProject::open + load_project. We just map the
        // return-enum to the Scriptable's outward contract.
        using R = Curvz::MainWindow::ScriptLoadResult;
        switch (m_main_window->script_load_project(path)) {
            case R::Ok:
                return ScriptValue::null();
            case R::Dragging:
                throw std::runtime_error(
                    "proj load: canvas drag in progress");
            case R::Dirty:
                // STRICTER than the GUI's on_open (which silently
                // replaces the project without check_unsaved_then —
                // arguably a GUI bug, logged separately). Refuse
                // and point the script author at the explicit save,
                // matching close's posture and message shape.
                // A future force_load verb (s250+) gives the
                // discard path.
                throw std::runtime_error(
                    "proj load: project has unsaved changes; "
                    "save first ('proj save' or 'proj save_as "
                    "<path>') or wait for a future force_load");
            case R::OpenFailed:
                // CurvzProject::open returned null. Conflates
                // missing directory / non-.curvz / parse failure
                // (open() doesn't distinguish them today). When
                // open() grows distinguishable error returns, the
                // enum can split without breaking the existing Ok
                // path.
                throw std::runtime_error(
                    "proj load: failed to open '" + path + "' "
                    "(directory missing, not a .curvz, or parse failed)");
        }
        // Unreachable — switch is exhaustive — but keeps the
        // compiler quiet about non-void return paths (mirrors save's,
        // save_as's, and close's tails).
        return ScriptValue::null();
    }

    if (verb == "new") {
        // s250 m1 — fifth verb on this Scriptable. Closes the m5b
        // proj-surface arc at 5 of 5 File-menu items. Same code
        // shape as load (one path arg, path_is_safe pre-flight,
        // MainWindow helper delegating to load_project); same
        // TestRunner-only mask as save_as, close, and load. Third
        // verb under the surface-preservation rule (close was
        // first, load was second; new is the third). Mechanically
        // distinct from load: load reads an existing .curvz via
        // CurvzProject::open, while new creates a fresh one via
        // CurvzProject::create_empty.
        //
        // Argument validation mirrors save_as and load exactly:
        // exactly one arg, coercible to string, non-empty. Wrong
        // shape is a structured throw (same posture as every
        // refusal branch on this Scriptable).
        if (args.size() != 1) {
            throw std::runtime_error(
                "proj new: expected exactly one path argument");
        }
        std::string path = arg_as_string(args[0]);
        if (path.empty()) {
            // Empty string OR wrong arg kind. Same combined message
            // as save_as and load; observable from the script
            // author's perspective is identical ("you didn't supply
            // a usable path").
            throw std::runtime_error(
                "proj new: path argument is empty or not a string");
        }

        // Path-containment pre-flight. THIRD production call site
        // for path_is_safe (save_as was first, load was second).
        // Same Scripter-unreachability note as save_as and load —
        // the dispatcher's RunContext gate refuses new BEFORE
        // invoke() runs (new's mask is ctx::TestRunner only;
        // Scripter is not in it). The branch is wired live for the
        // future TestRunner caller.
        //
        // Behaviour under path_is_safe: new creates a not-yet-
        // existing directory at the supplied path, so the
        // walk-up-to-existing-ancestor logic inside path_is_safe is
        // exercised here (same as save_as; load takes the simpler
        // direct-resolve path because its target must already
        // exist).
        std::string reason;
        if (!path_is_safe(path, &reason)) {
            throw std::runtime_error(
                "proj new: " + reason);
        }

        // Path cleared the safety gate. Delegate to MainWindow's
        // helper. The helper does drag check + dirty check +
        // target-exists check + CurvzProject::create_empty +
        // load_project. We just map the return-enum to the
        // Scriptable's outward contract.
        using R = Curvz::MainWindow::ScriptNewResult;
        switch (m_main_window->script_new_project(path)) {
            case R::Ok:
                return ScriptValue::null();
            case R::Dragging:
                throw std::runtime_error(
                    "proj new: canvas drag in progress");
            case R::Dirty:
                // STRICTER than the GUI's on_new_project (which
                // routes through check_unsaved_then — a modal the
                // script can't summon). Refuse and point the
                // script author at the explicit save, matching
                // load's posture and message shape. A future
                // force_new verb (s250+) gives the discard path.
                throw std::runtime_error(
                    "proj new: project has unsaved changes; "
                    "save first ('proj save' or 'proj save_as "
                    "<path>') or wait for a future force_new");
            case R::TargetExists:
                // STRICTER than the GUI's on_new_project (which
                // silently overwrites whatever's at the target).
                // This is the NEW refusal not present in load
                // (load wants the target to EXIST; new wants it
                // NOT to). Refuse and point the script author at
                // either a different path or, eventually, the
                // future force_new verb that gives the overwrite
                // path. Surfacing `path` in the message is the
                // useful diagnostic — most failures here will be
                // typos against an established directory.
                throw std::runtime_error(
                    "proj new: target '" + path + "' already exists; "
                    "use a different path or wait for a future "
                    "force_new");
            case R::CreateFailed:
                // CurvzProject::create_empty returned null. Conflates
                // directory-creation failure and the immediate save()-
                // inside-create_empty failure (create_empty() doesn't
                // distinguish them today). When create_empty grows
                // distinguishable error returns, the enum can split
                // without breaking the existing Ok path.
                throw std::runtime_error(
                    "proj new: failed to create project at '" + path +
                    "' (directory-creation or initial-save failed)");
        }
        // Unreachable — switch is exhaustive — but keeps the
        // compiler quiet about non-void return paths (mirrors save's,
        // save_as's, close's, and load's tails).
        return ScriptValue::null();
    }

    // Unknown verb — silent null, same posture as Inspector. The
    // listener doesn't currently flag unknown verbs (it does flag
    // unknown queries via the property() path); the surface stays
    // permissive so a typo doesn't trip an error counter.
    (void)args;
    return ScriptValue::null();
}

ScriptValue ProjScriptable::query(std::string_view property) const {
    // Queries are NOT RunContext-gated — reads can't corrupt
    // (CANON's RunContext entry: "Every model query. Reads can't
    // corrupt.").
    //
    // All three queries gracefully handle the no-project edge case
    // by returning the empty / false sentinel. has_path is the
    // smoke 44 precondition observable; dirty is the smoke 46
    // precondition observable (asserts dirty == true so the test
    // deterministically lands in the Dirty refusal branch).
    //
    // The queries read through MainWindow's script-side accessors
    // (script_project_path / script_project_dirty) — same private-
    // state-stays-private rationale as the invoke-side delegation
    // to script_save_project / script_save_project_as /
    // script_close_project.

    if (!m_main_window) {
        if (property == "has_path") return ScriptValue::boolean(false);
        if (property == "path")     return ScriptValue::text("");
        if (property == "dirty")    return ScriptValue::boolean(false);
        return ScriptValue::null();
    }

    if (property == "path") {
        // Empty string if no project, or project never saved.
        return ScriptValue::text(m_main_window->script_project_path());
    }
    if (property == "has_path") {
        // Boolean equivalent of `!path.empty()`. Assertable via the
        // DSL's equality-only `assert` form.
        return ScriptValue::boolean(
            !m_main_window->script_project_path().empty());
    }
    if (property == "dirty") {
        // s248 m1 — boolean undo-stack proxy. True iff a project is
        // loaded AND its command history has at least one undoable
        // entry. The same proxy on_close_project's check_unsaved_then
        // uses. See MainWindow::script_project_dirty for the
        // helper's contract; see this Scriptable's header for the
        // future "real" project-level dirty bit migration path
        // (helper body changes, query contract unchanged).
        return ScriptValue::boolean(
            m_main_window->script_project_dirty());
    }

    return ScriptValue::null();
}

std::vector<std::string> ProjScriptable::verbs() const {
    // s246 m1 / s247 m1 / s248 m1 / s249 m1 / s250 m1 — five verbs.
    // m5b proj-surface arc CLOSES at this point: all five File-menu
    // items (save / save_as / close / load / new) are addressable.
    // force_close / force_load / force_new land in subsequent
    // milestones when use cases name the need.
    return {
        "save",
        "save_as",
        "close",
        "load",
        "new",
    };
}

std::vector<std::string> ProjScriptable::properties() const {
    // s246 m1: path + has_path. s248 m1: + dirty (undo-stack proxy,
    // smoke 46's precondition observable; see header for the future
    // real-dirty-bit migration path).
    return {
        "path",
        "has_path",
        "dirty",
    };
}

// s246 m1 / s247 m1 / s248 m1 / s249 m1 / s250 m1 — RunContext mask declarations.
//
// `save`    — ctx::Scripter | ctx::TestRunner. Macro OUT (canon's
//             "Macro: not save_as <arbitrary>, not preferences
//             mutation" line covers save-flows as out-of-scope for
//             recorded automation; a recorded Macro overwriting the
//             user's project file without an explicit click trail
//             violates the user's mental model of what macros do).
//
// `save_as` — ctx::TestRunner only. Scripter and Macro both OUT.
//             Per CANON's RunContext pseudocode line `register_verb(
//             "save_as", ctx::TestRunner)` and the Scripter's "out
//             of scope" line: "`proj save_as <arbitrary-path>` —
//             use GUI Save As". The path-argument crosses a trust
//             boundary (script supplies arbitrary filesystem
//             targets) — even with path_is_safe gating the
//             destination to $HOME/$TMPDIR, the canon's posture is
//             that save_as belongs to the TestRunner surface, not
//             the in-app debugging playground.
//
// `close`   — ctx::TestRunner only. Scripter and Macro both OUT.
//             See header verb-surface block at `close` for the
//             surface-preservation rule: any verb whose successful
//             execution destroys or clears the script's own result
//             surface (output buffer, project, host window) is
//             TestRunner-only. The s248 m1 initial ship declared
//             close as Scripter | TestRunner by analogy to save's
//             mask shape; the trust boundary close actually crosses
//             is different from save's (save preserves observable
//             state; close destroys it), and the segfault observed
//             in testing surfaced the error. Sibling rule to path
//             containment; same RunContext enforcement layer.
//
// `load`    — ctx::TestRunner only. Scripter and Macro both OUT.
//             Second verb under the surface-preservation rule
//             (close was the first). A successful load destroys
//             the project the script's trace refers to AND seeds
//             a different one in its place. Same TestRunner-only
//             constant as save_as and close; no novel mask shape.
//
// `new`     — ctx::TestRunner only. Scripter and Macro both OUT.
//             Third verb under the surface-preservation rule
//             (close was first, load was second). A successful
//             new destroys any currently-loaded project and seeds
//             a fresh empty one in its place. Mechanically
//             distinct from load (creates rather than reads) but
//             the surface-preservation problem is identical:
//             trace continues against a different project than
//             earlier lines were written against. Same
//             TestRunner-only constant as save_as, close, and
//             load; no novel mask shape.
//
// Unknown verbs (typos, future verbs not yet declared) fall through
// to the base default of ctx::all_three. That matches the note in
// Scriptable::context_mask: "Unknown verb names (e.g. typos) SHOULD
// fall through to the default — the invoke() method itself is
// responsible for noticing the verb is unknown."
//
// Sixth consumer of context_mask() (Inspector + proj.save +
// proj.save_as + proj.close + proj.load + proj.new). Registry-
// promotion clock is at 6/n and STILL HELD by design — see
// ProjScriptable.hpp's context_mask declaration block for the
// at-six reasoning (new's mask is sibling to save_as's, close's,
// and load's; no novel mask shape, fourth instance of the same
// TestRunner-only constant within this Scriptable).
RunContextMask
ProjScriptable::context_mask(std::string_view verb) const {
    if (verb == "save") {
        return ctx::Scripter | ctx::TestRunner;
    }
    if (verb == "save_as") {
        return ctx::TestRunner;
    }
    if (verb == "close") {
        return ctx::TestRunner;
    }
    if (verb == "load") {
        return ctx::TestRunner;
    }
    if (verb == "new") {
        return ctx::TestRunner;
    }
    return ctx::all_three;
}

} // namespace curvz::scripting
