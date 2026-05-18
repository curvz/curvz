// scripting/ProjScriptable.hpp ───────────────────────────────────────────────
//
// s246 m1 — first headless-verb singleton from ARC m5b. Opens the Tier 4
// row (currently 0/~4-5). `proj` is the singleton that replaces the
// File-menu items routing through OS file pickers — `save`, `save_as`,
// `load`, `new`, `close`. m1 ships ONE verb, `save`, intentionally —
// the simplest of the five, to validate the singleton shape before the
// surface widens in subsequent milestones.
//
// s249 m1 ships the fourth verb: `load <path>`. First destructive-
// replacement verb in the proj surface (close tears down without
// replacing; load tears down and seeds with the next project).
// Second verb under the surface-preservation rule articulated by
// s248 m1 (close was the first). Sibling to save_as in code shape
// (one path arg, path_is_safe pre-flight, MainWindow helper
// delegating to an existing orchestrator); sibling to close in
// posture (TestRunner-only, refuses on Dirty by default — stricter
// than the current GUI on_open, which is arguably a GUI bug, see
// the load verb block for the rationale).
//
// s250 m1 ships the fifth and final File-menu verb: `new <path>`.
// Closes the m5b proj-surface arc at 5 of 5 File-menu items
// (save / save_as / close / load / new). Sibling to load in shape
// (one path arg, path_is_safe pre-flight, MainWindow helper sharing
// the load_project orchestrator with the GUI handler) and in
// posture (TestRunner-only by the surface-preservation rule when
// replacing an existing project; also under the same Dirty
// stricter-than-GUI refusal posture). Mechanically distinct from
// load: load reads an EXISTING .curvz from disk, while new
// CREATES a fresh one at a not-yet-existing path. The construction
// difference introduces one new structural refusal — TargetExists
// — that load doesn't have (load wants the path to exist; new
// wants it not to).
//
// See CANON.md "Headless-verb singletons" for the design rule this
// implements (the substrate can address the GUI Save button but not
// the resulting picker; scripts leapfrog the picker by passing the
// argument directly — here, `save` doesn't even need an argument,
// because the project already knows its own directory).
//
// ── Why this is a singleton (not a per-document Scriptable) ────────────────
//
// A project is a process-scope concept — there's at most one open at a
// time (Curvz's MainWindow holds a single `m_project`). The "save the
// project" outcome doesn't address any particular document inside the
// project; it addresses the project AS A WHOLE. A per-document
// Scriptable would have to either name the project as a special doc
// (wrong shape — the project isn't a document) or sprout a sibling
// proxy that's collection-of-one (overkill for cardinality 1).
//
// Sibling to InspectorScriptable in the shape sense — flat verb
// surface, no per-instance proxies, MainWindow-pointer constructor,
// held by MainWindow as a unique_ptr member.
//
// ── Addressing ─────────────────────────────────────────────────────────────
//
//   proj                    — the Scriptable. ONE registry entry per
//                             app session; held as a member by
//                             MainWindow, registered for the lifetime
//                             of the window. No per-instance proxy.
//
// `can_resolve` / `proxy_for` are unimplemented (inherit base no-op).
//
// ── Verb surface (s246 m1: `save`; s247 m1: + `save_as`; s248 m1: + `close`;
//                 s249 m1: + `load`; s250 m1: + `new`) ────────────────────
//
//   save                    — write the project to its current
//                             directory. No arguments. The path is
//                             implicit (already set by an earlier
//                             user action through the GUI Save As
//                             picker or by Project::create_new /
//                             Project::open at project-construction
//                             time, both of which require an explicit
//                             user trust signal).
//
//                             Contract — three refusal paths and one
//                             happy path:
//
//                             - No project loaded (m_project null):
//                               refuse with structured error. Rare
//                               edge case; can occur transiently
//                               between document close and open in
//                               a future state machine.
//
//                             - Project loaded but directory empty
//                               (m_project->directory.empty()): refuse
//                               with structured error pointing the
//                               user to `proj save_as <path>` (s247).
//                               This is the "would-fall-through-to-
//                               picker" branch — CANON's headless-
//                               verb-singletons rule says leapfrog the
//                               picker by passing the argument
//                               directly; save's argument is implicit
//                               in the project's directory state, so
//                               the absence of that state is the
//                               refusal trigger.
//
//                             - Canvas drag in flight
//                               (m_canvas.is_dragging()): refuse with
//                               structured error. The GUI's `on_save`
//                               handler defers via a 100ms
//                               signal_timeout in this case (mid-drag
//                               saves can corrupt node data), but a
//                               Scriptable verb that returns `ok`
//                               synchronously while a deferred save
//                               fires later would lie about its
//                               result. Refusing lets the script
//                               re-issue cleanly. Rare in practice —
//                               script verbs run synchronously and
//                               can't easily interleave with user
//                               drag gestures.
//
//                             - Otherwise: call CurvzProject::save().
//                               On true, update_title() + LOG_INFO,
//                               return Null (listener prints `ok`).
//                               On false, return structured error
//                               (the IO layer's failure path).
//
//                             RunContext mask:
//                                 ctx::Scripter | ctx::TestRunner
//                             — Macro is OUT. CANON's RunContext
//                             entry names "`save_as <arbitrary>` not
//                             Macro" and a recorded-Macro save
//                             overwriting user work without an
//                             explicit click trail violates the
//                             user's mental model. See CANON's
//                             RunContext pseudocode for the canonical
//                             declaration form (register_verb("save",
//                             TestRunner | Scripter)).
//
//   save_as <path>          — s247 m1. Write the project to the
//                             supplied path. First path-argument verb
//                             on this Scriptable; first live exercise
//                             of curvz::scripting::path_is_safe under
//                             user-supplied input.
//
//                             Contract — five refusal paths and one
//                             happy path:
//
//                             - Wrong argument shape (zero args, more
//                               than one arg, or arg is not a string /
//                               not coercible to one): refuse with
//                               structured error. Argument validation
//                               fires at the Scriptable layer before
//                               any state inspection.
//
//                             - Path-containment refusal (path_is_safe
//                               returns false): refuse with structured
//                               error carrying the reason_out string
//                               from path_is_safe ("outside $HOME and
//                               $TMPDIR", "realpath failed", "empty
//                               path", "path-containment not
//                               initialised"). NOTE: from Scripter
//                               context this branch is structurally
//                               unreachable — the dispatcher's
//                               RunContext gate refuses save_as
//                               BEFORE invoke() runs (Scripter is not
//                               in the mask). The branch is wired
//                               live for the future TestRunner caller.
//
//                             - No project loaded (m_project null):
//                               refuse with structured error. Same
//                               edge case as save.
//
//                             - Canvas drag in flight: refuse with
//                               structured error. Same rationale as
//                               save — synchronous ok-echo while a
//                               deferred write fires later would lie
//                               about the result.
//
//                             - Otherwise: delegate to
//                               MainWindow::script_save_project_as(path),
//                               which assigns the directory and calls
//                               do_save_as → CurvzProject::save(). On
//                               true, return Null (listener prints
//                               `ok`). On false, return structured
//                               error ("save failed").
//
//                             RunContext mask:
//                                 ctx::TestRunner
//                             — TestRunner ONLY. CANON's RunContext
//                             pseudocode lists `register_verb(
//                             "save_as", ctx::TestRunner)` and the
//                             Scripter's "out of scope" line names
//                             "proj save_as <arbitrary-path>" as
//                             use-GUI-Save-As territory.
//
//                             **Third consumer of context_mask()** —
//                             registry-promotion clock hits 3/3.
//                             Default call is HOLD; see s247 handoff
//                             "Architectural forks worth surfacing"
//                             fork 1 for the rationale (virtual
//                             scales fine at 3 consumers, promotion
//                             would touch 8 + 2 + substrate, no
//                             empirical pain yet).
//
//   close                   — s248 m1. Close the currently-loaded
//                             project. No arguments. Sibling to
//                             `save` in mask shape; sibling to
//                             on_close_project in teardown shape.
//
//                             Contract — four refusal paths and one
//                             happy path:
//
//                             - No project loaded (m_project null):
//                               refuse with structured error. Closing
//                               "nothing" is a no-op disguised as a
//                               success; refuse so the script author
//                               sees their assumption ("a project is
//                               loaded") was wrong.
//
//                             - Canvas drag in flight: refuse with
//                               structured error. Same rationale as
//                               save / save_as — synchronous ok-echo
//                               while a deferred teardown fires later
//                               would lie about the result.
//
//                             - Project has unsaved work
//                               (m_history.can_undo() true): refuse
//                               with structured error pointing the
//                               script author at `proj save` (or
//                               `proj save_as <path>` for never-saved
//                               projects). The GUI's on_close_project
//                               handles this via check_unsaved_then,
//                               which summons a Save/Discard/Cancel
//                               modal — Scripts can't summon modals
//                               (CANON's headless-verb-singletons
//                               rule), so the modal becomes a
//                               structured refusal. Same picker-
//                               fallthrough conversion as save's
//                               NoPath refusal (s246 m1).
//
//                               No "force close" / "discard" verb
//                               ships in m1. The script author who
//                               genuinely wants to discard work has
//                               two paths: (a) clear the undo stack
//                               by re-doing the same work elsewhere
//                               (awkward), (b) wait for a future
//                               force_close verb or close-with-arg
//                               flag (s250+). Refusing rather than
//                               silently discarding is the safer
//                               default — accidentally losing the
//                               user's work via a script that didn't
//                               check dirty first is a bug class
//                               worth refusing eagerly to avoid.
//
//                             - Otherwise: delegate to
//                               MainWindow::script_close_project(),
//                               which delegates the teardown work to
//                               do_close_project() (the same method
//                               on_close_project's check_unsaved_then
//                               lambda body lives in as of s248 m1).
//                               Return Null (listener prints `ok`).
//
//                             RunContext mask:
//                                 ctx::TestRunner
//                             — TestRunner ONLY. Scripter and Macro
//                             both OUT.
//
//                             *** WHY THIS IS NOT SCRIPTER-CALLABLE ***
//                             *** despite no path argument        ***
//
//                             The Scripter playground hosts the
//                             script's output buffer. The script's
//                             trace — every `>` echo, every `= value`
//                             return, every PASS/FAIL line, every
//                             summary count — accumulates in that
//                             buffer for the operator to read AFTER
//                             the script finishes.
//
//                             do_close_project's teardown invalidates
//                             every panel, including state the
//                             Scripter window depends on for its
//                             output rendering. Even where the
//                             teardown happens to leave the buffer
//                             intact in memory, subsequent lines of
//                             the same script that read project
//                             state (`get proj has_path` /
//                             `get proj dirty`) execute against a
//                             demolished MainWindow — and the
//                             observed effect at s248 m1 ship was
//                             SIGSEGV.
//
//                             Even WITH the segfault patched, a
//                             successful close from Scripter would
//                             still mean the script's results are
//                             unobservable AFTER the run: the buffer
//                             might survive but the project state
//                             the trace refers to is gone, and there
//                             is no current "copy results to
//                             clipboard before destroying" verb.
//                             Requiring the script author to remember
//                             to bookmark output before calling
//                             close is exactly the kind of trap the
//                             substrate is supposed to make impossible
//                             by design, not by discipline.
//
//                             *** SURFACE PRESERVATION ***
//
//                             The general rule that emerges: any verb
//                             whose successful execution destroys or
//                             clears the script's own RESULT SURFACE
//                             (the Scripter's output buffer, the
//                             project the script is reasoning about,
//                             the listener's host window) is
//                             TestRunner-only. This is the structural
//                             sibling to path containment, with the
//                             same RunContext enforcement layer:
//
//                                - Path containment protects the
//                                  filesystem from scripts.
//                                - Surface preservation protects the
//                                  script's readable output from the
//                                  script itself.
//
//                             Sensitive verbs by this rule:
//                                - proj close, proj load, proj new
//                                  (mutate or destroy the project
//                                  the script reasons about)
//                                - future app quit, app reset (kill
//                                  the host)
//                                - future clear_output (wipes the
//                                  buffer directly)
//
//                             The Scripter's stable long-term `proj`
//                             surface is therefore: `save` (writes
//                             but preserves observable state and
//                             output) plus the read-only queries
//                             (`path`, `has_path`, `dirty`).
//                             Everything that mutates project
//                             identity is TestRunner territory.
//
//                             Macro is OUT for the same Macro-as-
//                             surprising-destroyer reason save's
//                             Macro is OUT.
//
//                             **Fourth consumer of context_mask()** —
//                             clock goes 3/3 → 4/n. Promotion still
//                             HELD by design (see context_mask()
//                             declaration block below).
//
//   load <path>             — s249 m1. Replace the currently-loaded
//                             project with the one at the supplied
//                             path. First destructive-replacement
//                             verb (close tears down without
//                             replacing; load tears down AND seeds
//                             with the next project). Sibling to
//                             save_as in shape (one path arg,
//                             path_is_safe pre-flight, MainWindow
//                             helper that delegates to an existing
//                             orchestrator); sibling to close in
//                             posture (TestRunner-only, by the
//                             surface-preservation rule).
//
//                             Contract — five refusal paths and one
//                             happy path:
//
//                             - Wrong argument shape (zero args, more
//                               than one arg, or arg not coercible to
//                               a non-empty string): refuse with
//                               structured error. Same arg-validation
//                               posture as save_as (the script
//                               author needs to see why nothing was
//                               loaded; silent-null would hide the
//                               typo).
//
//                             - Path-containment refusal (path_is_safe
//                               returns false): refuse with structured
//                               error carrying the reason_out string
//                               from path_is_safe. Second production
//                               call site for path_is_safe (save_as
//                               was the first). Same Scripter-
//                               unreachability note as save_as — the
//                               dispatcher gate refuses load from
//                               Scripter BEFORE invoke() runs; the
//                               branch is wired live for the future
//                               TestRunner caller.
//
//                             - Canvas drag in flight: refuse with
//                               structured error. Same rationale as
//                               every project-lifecycle verb.
//
//                             - Project has unsaved work
//                               (m_history.can_undo() true): refuse
//                               with structured error pointing the
//                               script author at `proj save` (or
//                               `proj save_as <path>`).
//
//                               IMPORTANT: this is STRICTER than the
//                               current GUI on_open behaviour. The
//                               GUI silently replaces the project
//                               without running check_unsaved_then —
//                               which is arguably a GUI bug
//                               (separate backlog item), but
//                               regardless, the script-side stricter
//                               posture is the right default: a
//                               GUI replacement is visually obvious
//                               (title bar, panels redraw), but a
//                               script silently destroying mid-
//                               automation is exactly the discipline-
//                               instead-of-design trap surface
//                               preservation is here to prevent.
//                               No "force_load" verb in m1; same
//                               banked-for-when-named posture as
//                               force_close.
//
//                             - Open failure (CurvzProject::open
//                               returned null — directory doesn't
//                               exist, isn't a .curvz, parse failed):
//                               refuse with structured error. The
//                               three failure causes are conflated
//                               because open() itself doesn't
//                               distinguish them today.
//
//                             - Otherwise: delegate to
//                               MainWindow::script_load_project(path),
//                               which delegates the actual project
//                               swap to load_project() (the same
//                               orchestrator on_open calls inside its
//                               file-dialog lambda). Return Null
//                               (listener prints `ok`).
//
//                             RunContext mask:
//                                 ctx::TestRunner
//                             — TestRunner ONLY. Scripter and Macro
//                             both OUT, by the same surface-
//                             preservation rule that applies to
//                             close: a successful load destroys the
//                             project the script's trace refers to
//                             and tears down the host window state
//                             the Scripter depends on for output
//                             rendering. Sibling rule to path
//                             containment; same enforcement layer.
//                             load is the second verb shipped under
//                             this rule (close was the first;
//                             save_as is sibling but under the
//                             path-trust-boundary sibling rule).
//
//                             **Fifth consumer of context_mask()** —
//                             clock goes 4/n → 5/n. Promotion STILL
//                             HELD by design (see context_mask()
//                             declaration block below): load's mask
//                             (TestRunner only) is sibling to
//                             save_as's and close's — same constant
//                             applied to a third verb-name string,
//                             no novel mask shape earned.
//
//   new <path>              — s250 m1. Create a fresh empty project
//                             at the supplied path and load it,
//                             replacing the currently-loaded project
//                             if any. Closes the m5b proj-surface
//                             arc at 5 of 5 File-menu items
//                             (save / save_as / close / load / new).
//                             Sibling to load in code shape (one
//                             path arg, path_is_safe pre-flight,
//                             MainWindow helper sharing the
//                             load_project orchestrator with the
//                             GUI handler); sibling to load in
//                             posture (TestRunner-only by the
//                             surface-preservation rule when
//                             replacing an existing project; same
//                             stricter-than-GUI Dirty refusal).
//
//                             Contract — six refusal paths and one
//                             happy path. ONE more refusal than load
//                             because new introduces a target-
//                             existence refusal (load wants the
//                             path to exist; new wants it not to):
//
//                             - Wrong argument shape (zero args, more
//                               than one arg, or arg not coercible to
//                               a non-empty string): refuse with
//                               structured error. Same arg-validation
//                               posture as save_as and load.
//
//                             - Path-containment refusal (path_is_safe
//                               returns false): refuse with structured
//                               error carrying the reason_out string
//                               from path_is_safe. Third production
//                               call site for path_is_safe (save_as,
//                               load were the first two). Same
//                               Scripter-unreachability note as
//                               save_as and load — the dispatcher
//                               gate refuses new from Scripter BEFORE
//                               invoke() runs (new's mask is
//                               ctx::TestRunner only; Scripter is
//                               not in it). The branch is wired
//                               live for the future TestRunner
//                               caller.
//
//                               Behaviour under path_is_safe: new
//                               creates a not-yet-existing directory
//                               at the supplied path, so the
//                               path_is_safe walk-up-to-existing-
//                               ancestor logic is exercised here
//                               (mirrors save_as more than load —
//                               load wants the target to exist
//                               already; save_as and new both write
//                               to not-yet-existing targets).
//
//                             - Canvas drag in flight: refuse with
//                               structured error. Same rationale as
//                               every project-lifecycle verb.
//
//                             - Project has unsaved work
//                               (m_history.can_undo() true): refuse
//                               with structured error pointing the
//                               script author at `proj save` (or
//                               `proj save_as <path>`). Same
//                               STRICTER-than-GUI posture as load:
//                               the GUI's on_new_project routes
//                               through check_unsaved_then (a modal
//                               the script can't summon), so the
//                               script-side structural refusal is
//                               the modal's equivalent. A future
//                               force_new verb (s250+) gives the
//                               discard path.
//
//                             - Target already exists
//                               (fs::exists(path) true BEFORE
//                               create_empty runs): refuse with
//                               structured error. STRICTER than the
//                               GUI's on_new_project, which silently
//                               overwrites an existing target — the
//                               GUI calls create_empty against
//                               whatever the picker returns,
//                               clobbering any project.json /
//                               swatches.json / styles.json /
//                               themes.json that may already live
//                               there. The script-side stricter
//                               posture is deliberate by the same
//                               rationale as the Dirty refusal: a
//                               script silently destroying an
//                               existing project mid-automation is
//                               exactly the surface-preservation
//                               problem in another costume.
//
//                               This is a NEW refusal not present
//                               in load (load needs the target to
//                               EXIST; new needs it NOT to). The
//                               structural asymmetry is genuine
//                               and the enum carries an extra
//                               variant — see ScriptNewResult in
//                               MainWindow.hpp.
//
//                               A future force_new verb (or
//                               new --replace flag) would give the
//                               overwrite path. Not in m1 by the
//                               same banked-for-when-named posture
//                               as force_close / force_load.
//                               The GUI's silent-overwrite is
//                               logged as a separate backlog item
//                               (the third "GUI is laxer than
//                               script" gap, alongside on_open's
//                               missing check_unsaved_then and
//                               on_save_as's only-confirm-on-file-
//                               clash — though save_as at least
//                               does have a confirm dialog, which
//                               on_new_project doesn't).
//
//                             - Create failed (CurvzProject::
//                               create_empty returned null —
//                               directory creation failed or the
//                               immediate save() inside
//                               create_empty failed): refuse with
//                               structured error. The two failure
//                               causes are conflated because
//                               create_empty itself doesn't
//                               distinguish them today.
//
//                             - Otherwise: delegate to
//                               MainWindow::script_new_project(path),
//                               which delegates the actual project
//                               swap to load_project() (the same
//                               orchestrator on_new_project calls
//                               inside its file-dialog lambda).
//                               Return Null (listener prints `ok`).
//
//                             RunContext mask:
//                                 ctx::TestRunner
//                             — TestRunner ONLY. Scripter and Macro
//                             both OUT, by the same surface-
//                             preservation rule that applies to
//                             close and load: a successful new
//                             destroys any currently-loaded project
//                             and seeds a fresh empty one in its
//                             place. new is the third verb shipped
//                             under this rule (close was first,
//                             load was second). Same TestRunner-only
//                             constant; no novel mask shape.
//
//                             **Sixth consumer of context_mask()** —
//                             clock goes 5/n → 6/n. Promotion STILL
//                             HELD by design (see context_mask()
//                             declaration block below): new's mask
//                             (TestRunner only) is sibling to
//                             save_as's, close's, and load's —
//                             same constant applied to a fourth
//                             verb-name string within this
//                             Scriptable, no novel mask shape
//                             earned.
//
// ── Query surface (s246 m1: `path`, `has_path`; s248 m1: + `dirty`) ────────
//
//   path                    — String. The project's directory path
//                             (CurvzProject::directory), or empty
//                             string if no project is loaded or the
//                             project has never been saved.
//
//   has_path                — Boolean. True iff `path` is non-empty.
//                             Assertable via the DSL's equality-only
//                             `assert` form. The smoke's primary
//                             happy-path observable.
//
//   dirty                   — s248 m1. Boolean. True iff a project
//                             is loaded AND m_history.can_undo()
//                             returns true (the project has work on
//                             its undo stack). False otherwise (no
//                             project, or a project with an empty
//                             history).
//
//                             Uses the same proxy on_close_project's
//                             check_unsaved_then uses — Curvz does
//                             not have a true project-level dirty bit
//                             today, but the undo-stack signal IS
//                             what the GUI uses end-to-end, so the
//                             script-side semantic stays in lock-step.
//                             When a real project-level dirty bit
//                             lands (backlog), the helper's body
//                             changes; the public query contract is
//                             unchanged.
//
//                             Smoke 46's precondition observable —
//                             the test asserts dirty == true before
//                             calling proj close, so the test runs
//                             in the Dirty branch deterministically
//                             (without the query, the smoke could
//                             only operator-visual the precondition).
//
// Both reads only; no RunContext gating (queries are not gated by
// canon: "Every model query. Reads can't corrupt.").
//
// ── Out of scope for s250 m1 ───────────────────────────────────────────────
//
// **`force_close` / `close --discard`** — close a dirty project
// without saving, losing the unsaved work. The Discard branch of
// the GUI's check_unsaved_then dialog. Not in m1 because no use
// case has named the need yet, and the structural refusal Dirty is
// the right default until one does. When it lands, the shape is
// probably a sibling verb (force_close, distinct mask) rather than
// an arg flag (close --discard) — distinct verbs declare distinct
// masks cleanly, and "yes I'm sure" verbs reading distinctly in
// the script source helps the author notice they're choosing the
// destructive path.
//
// **`force_load` / `load --discard`** — load a different project
// while discarding any unsaved work in the current one. Same
// banked-for-when-named posture as force_close. The s249 m1
// Dirty refusal becomes the structural opt-in path: "to load
// anyway, call `proj save` first, or wait for a future
// force_load verb."
//
// **`force_new` / `new --replace`** — create a fresh project at a
// path that already exists, overwriting any existing project there.
// The opt-in path for the s250 m1 TargetExists refusal. Same
// banked-for-when-named posture as force_close and force_load.
// The shape will probably be a sibling verb (force_new, distinct
// mask) for the same "yes I'm sure" reading discipline; the
// overlap with force_load + dirty's opt-in is interesting (a
// force_new against an existing project is BOTH "overwrite the
// existing project" AND "discard my current unsaved work"), but
// the two opt-ins read distinctly because they correspond to
// distinct refusal sources — the script author who hits both
// refusals reaches for both force verbs in sequence rather than
// one combined verb that papers over the distinction.
//
// ── Lifetime ───────────────────────────────────────────────────────────────
//
// MainWindow* is captured as a raw pointer at construction; MainWindow
// outlives the Scriptable (the Scriptable is a member of MainWindow,
// destroyed in its dtor). No project getter needed — m_project lives
// on MainWindow, and we resolve it through the pointer on every
// invoke / query (matches the comment in MainWindow.hpp's project-
// member block at line 645).

#pragma once
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptValue.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Curvz {
class MainWindow;
}

namespace curvz::scripting {

class ProjScriptable : public Scriptable {
public:
    // Registers as "proj" via the Scriptable base ctor.
    // `main_window` is non-owning; the Scriptable is held as a member
    // of MainWindow and destroyed in MainWindow's dtor, so the pointer
    // is valid for the Scriptable's entire lifetime. nullptr would
    // be a bug; we don't accept it as a defensive option — caller
    // must pass a real pointer.
    explicit ProjScriptable(Curvz::MainWindow* main_window);
    ~ProjScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // s246 m1 / s247 m1 / s248 m1 / s249 m1 / s250 m1 — RunContext mask declarations.
    //
    //   save     — ctx::Scripter | ctx::TestRunner (Macro is OUT —
    //              per CANON's RunContext pseudocode and the
    //              Scripter's "in-scope" line listing `proj save` as
    //              the "commit my repro setup" verb).
    //   save_as  — ctx::TestRunner (Scripter is OUT — per CANON's
    //              RunContext pseudocode line `register_verb(
    //              "save_as", ctx::TestRunner)` and the Scripter's
    //              "out of scope" line naming "proj save_as
    //              <arbitrary-path>" as use-GUI-Save-As territory.
    //              Macro is OUT for the same reason save's Macro is
    //              OUT, with extra weight since save_as also crosses
    //              the path-argument trust boundary).
    //   close    — ctx::TestRunner only. Scripter and Macro both
    //              OUT. The surface-preservation rule (see header
    //              verb-surface block at `close`): any verb whose
    //              successful execution destroys or clears the
    //              script's own result surface — its output buffer,
    //              the project it reasons about, the listener's host
    //              window — is TestRunner-only. close destroys the
    //              project AND tears down state the Scripter window
    //              depends on for output rendering. The s248 m1
    //              initial ship mistakenly declared close as
    //              `ctx::Scripter | ctx::TestRunner` reasoning by
    //              analogy to save's mask shape; the actual trust
    //              boundary at stake is different from save's, and
    //              the segfault observed in testing surfaced the
    //              error. Corrected to ctx::TestRunner in s248 m1's
    //              fix re-ship. Sibling rule to path containment;
    //              same RunContext enforcement layer.
    //   load     — ctx::TestRunner only. Scripter and Macro both
    //              OUT. Second verb under the surface-preservation
    //              rule (close was first). A successful load
    //              destroys the project the script's trace refers
    //              to AND replaces it with a different one — the
    //              surface preservation problem applies to BOTH the
    //              destruction (the close-style segfault risk) and
    //              the replacement (the script's trace would
    //              continue against the new project, which the
    //              earlier lines weren't written against). Same
    //              TestRunner-only constant as save_as and close;
    //              no novel mask shape.
    //   new      — ctx::TestRunner only. Scripter and Macro both
    //              OUT. Third verb under the surface-preservation
    //              rule (close was first, load was second). A
    //              successful new destroys any currently-loaded
    //              project and seeds a fresh empty one in its
    //              place. Mechanically distinct from load (writes
    //              new disk state rather than reading existing
    //              state) but the surface-preservation problem is
    //              identical: the script's trace continues against
    //              a different project than the earlier lines were
    //              written against. Same TestRunner-only constant
    //              as save_as, close, and load; no novel mask
    //              shape.
    //
    // Future verbs (`force_close`, `force_load`, `force_new`) will
    // declare their own masks here when they land. See
    // scripting/RunContext.hpp for the enum and helper constants;
    // see CANON's "RunContext gates the verb surface" entry for the
    // declaration discipline.
    //
    // NOTE: with `new` shipping, this is the SIXTH consumer of
    // context_mask() (Inspector + this Scriptable's `save` +
    // `save_as` + `close` + `load` + `new`). The registry-promotion
    // clock is now at 6/n.
    //
    // **Decision (s250 m1): STILL HOLD the promotion.** Same
    // reasoning as at 5/n: new's mask (TestRunner-only) is sibling
    // to save_as's, close's, and load's — same constant applied to
    // a fourth verb-name string within this Scriptable. No novel
    // mask combination has appeared. The same-shape argument that
    // applied at 3/3 still applies at 6/n.
    //
    // The catalogue across this Scriptable is now markedly
    // uniform: one verb at the Scripter | TestRunner mask (save),
    // four verbs at the TestRunner-only mask (save_as, close,
    // load, new). That uniformity strengthens the argument FOR a
    // future promotion — when 80% of the verbs share a constant,
    // hoisting it to a registry would dedupe the comment blocks
    // even more than the if-branches. The argument AGAINST
    // promotion-today is also unchanged: no typo bug has bitten,
    // the catalogue hasn't grown past ~8 declarations, and no
    // future consumer with a genuinely new mask shape has appeared
    // (Macro-only verb, "TestRunner + special caller" combination,
    // anything novel — new doesn't count, it just adds the fourth
    // instance of the same TestRunner-only mask). Banked
    // observation. See s248 handoff "Architectural forks" for the
    // at-four reasoning; s249 / s250 don't change the call.
    RunContextMask context_mask(std::string_view verb) const override;

private:
    Curvz::MainWindow* m_main_window;
};

} // namespace curvz::scripting
