// scripting/ExportScriptable.hpp ─────────────────────────────────────────────
//
// s251 m1 — `export` Scriptable. SECOND headless-verb singleton from ARC
// m5b (Tier 4 row ticks 1/~4-5 → 2/~4-5). Opens the second slot in the
// row after `proj` closed at 5/5 File-menu verbs (s246 m1 through s250
// m1). Same singleton-host pattern as `inspector` (s222 m2) and `proj`
// (s246 m1): flat verb surface, no proxy routing, MainWindow-pointer
// constructor, held by MainWindow as a unique_ptr member.
//
// ── Why this is the natural next singleton after `proj` ─────────────────────
//
// CANON's "Headless-verb singletons" entry names one singleton per
// concern. The proj singleton wraps the project-level lifecycle (the
// File-menu items routing through OS file pickers). `export` is the
// next picker-coupled concern in the GUI — Project ▸ Export Documents
// opens a dialog whose Accept button routes through five distinct
// format-specific writers (`SvgWriter::write_svg_file*`, `export_png_*`,
// `RefptExporter::export_refpts`, `IcoExporter` family, `GResourceExporter`).
// The substrate can address the dialog's controls, but not the resulting
// file-format-specific picker that runs at Accept; the script leapfrogs
// the picker by supplying the argument directly to the verb.
//
// One singleton per concern, NOT per file format. `export svg`, `export
// png`, `export ico`, `export refpt`, `export gresource` are all
// FORMATS OF EXPORTING — one concern, five behaviours. CANON's entry
// names this explicitly: "Cardinality is by concern, not by file format.
// `export` is one Scriptable; the per-format machinery lives inside the
// verb body." So this Scriptable will host all five formats as separate
// verbs across subsequent milestones (m1 ships svg only; m2+ adds png /
// ico / refpt / gresource).
//
// ── Why this is a singleton (not a per-document Scriptable) ────────────────
//
// "Export the active doc to <path>" is a process-scope operation that
// addresses the project's currently-active document by implication, not
// by argument. The per-doc question ("export THIS doc, not that one")
// is answered by which doc is active when the script runs — same as
// `proj save` writes the currently-loaded project, not a script-named
// one. A per-document Scriptable would have to either name the export
// outcome as a per-doc verb (`doc.<iid> export_svg <path>`) or sprout a
// sibling Scriptable per format — both wrong shapes. The export
// CONCERN belongs at the singleton level; the doc identity is implicit
// in active state.
//
// Sibling to InspectorScriptable and ProjScriptable in the shape sense
// — flat verb surface, no per-instance proxies, MainWindow-pointer
// constructor.
//
// ── Addressing ─────────────────────────────────────────────────────────────
//
//   export                  — the Scriptable. ONE registry entry per
//                             app session; held as a member by
//                             MainWindow, registered for the lifetime
//                             of the window. No per-instance proxy.
//
// `can_resolve` / `proxy_for` are unimplemented (inherit base no-op).
//
// ── Verb surface (s251 m1: `svg`) ──────────────────────────────────────────
//
//   svg <path>              — write the project's ACTIVE DOCUMENT as
//                             a standalone .svg file at the supplied
//                             path. The first format-verb in the
//                             singleton; subsequent formats (png, ico,
//                             refpt, gresource) land in m2+ as the
//                             surface widens.
//
//                             Contract — five refusal paths and one
//                             happy path:
//
//                             - Wrong argument shape (zero args, more
//                               than one arg, or arg is not a string /
//                               not coercible to one): refuse with
//                               structured error. Argument validation
//                               fires at the Scriptable layer before
//                               any state inspection. Same posture as
//                               proj save_as's wrong-arg refusal.
//
//                             - Path-containment refusal (path_is_safe
//                               returned false): refuse with the reason
//                               string from path_is_safe (audit-log-
//                               friendly: "outside $HOME and $TMPDIR",
//                               "realpath failed (no resolvable
//                               ancestor)", "empty path", or "path-
//                               containment not initialised"). Fourth
//                               production call site of
//                               curvz::scripting::path_is_safe (after
//                               save_as s247 m1, load s249 m1, new
//                               s250 m1). The first three were all on
//                               ProjScriptable; this is the first
//                               call site on a DIFFERENT Scriptable —
//                               confirming the utility's design intent
//                               (one validator, many callers, no
//                               per-verb path logic).
//
//                             - No project loaded (m_project null in
//                               MainWindow): refuse with structured
//                               error. Same rare edge case as proj
//                               save's NoProject branch.
//
//                             - No active document
//                               (m_project->active_doc() returns
//                               nullptr): refuse with structured
//                               error. Distinct from NoProject — a
//                               project can exist with zero documents
//                               transiently (the last doc was just
//                               removed via DocTabBar's close button).
//                               Mirrors the GUI's empty-gallery state
//                               which would render the Export menu
//                               item insensitive.
//
//                             - I/O failure (SvgWriter::write_svg_file
//                               returned false — couldn't open the
//                               output path for writing, or the close/
//                               flush failed): refuse with structured
//                               error. Same posture as proj save's
//                               IoFailed.
//
//                             - Otherwise: write the active doc as
//                               SVG to `path` via write_svg_file().
//                               Update last_path / last_ok members
//                               for the smoke's assertable observable.
//                               Return Null (listener prints `ok`).
//
//                             RunContext mask:
//                                 ctx::Scripter | ctx::TestRunner
//                             — Macro is OUT. This is the SAME mask
//                             shape as proj save (not proj save_as).
//                             Rationale: export produces a SIDE
//                             artefact at a path, it does not rewrite
//                             the project's own identity. CANON's
//                             Scripter-in-scope list explicitly
//                             includes "`/tmp` writes for scratch
//                             and diagnostic dumps" — exporting an
//                             SVG to /tmp for visual inspection IS
//                             the canonical use case. The trust
//                             boundary that makes proj save_as
//                             TestRunner-only (rewriting user-data
//                             identity to an arbitrary disk
//                             location) does not apply to export
//                             (the project is unchanged; only a
//                             side artefact lands on disk).
//
//                             Macro is OUT for the same reason proj
//                             save is OUT of Macro: a recorded
//                             automation writing arbitrary file
//                             paths without an explicit click trail
//                             violates the user's mental model of
//                             what macros do. The path-containment
//                             gate ($HOME + $TMPDIR) covers the
//                             system-integrity layer; RunContext's
//                             Macro exclusion covers the user-data
//                             layer's "recorded automation should
//                             not silently write files" rule.
//
// ── Query surface (s251 m1: `last_path`, `last_ok`) ────────────────────────
//
//   last_path  — text. Path of the most recent SUCCESSFUL export. Empty
//                string if no export has succeeded yet this session
//                (initial state) or if every attempted export failed.
//                Read-only.
//
//   last_ok    — boolean. true iff the most recent export call
//                succeeded. false at session start (no exports yet) and
//                after any failed export. Read-only.
//
// Both queries are members of the Scriptable itself (not delegated to
// MainWindow) — they describe the most recent ATTEMPT, not project
// state, and the natural home for "what just happened on this
// Scriptable" is the Scriptable. Mirrors how the s216+ Scriptables
// hold their own per-instance state (the iid of the last-selected
// proxy, etc.) rather than reaching through MainWindow for every
// query.
//
// These are the SMOKE'S ASSERTABLE OBSERVABLES — the DSL's `assert`
// is equality-only on query results, so a successful export's
// observable proof must surface through a query. `proj save`'s
// post-save sanity uses `proj has_path`; export's analogue is
// `export last_path` (equal to the path just supplied) and
// `export last_ok` (== true).
//
// ── Lifetime ───────────────────────────────────────────────────────────────
//
// MainWindow* is captured as a raw pointer at construction; MainWindow
// outlives the Scriptable (the Scriptable is a member of MainWindow,
// destroyed in its dtor). No project getter needed — m_project lives
// on MainWindow, and we resolve it through the pointer on every
// invoke / query. Same lifetime story as InspectorScriptable and
// ProjScriptable.

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

class ExportScriptable : public Scriptable {
public:
    // Registers as "export" via the Scriptable base ctor.
    // `main_window` is non-owning; the Scriptable is held as a member
    // of MainWindow and destroyed in MainWindow's dtor, so the pointer
    // is valid for the Scriptable's entire lifetime. nullptr would
    // be a bug; we don't accept it as a defensive option — caller
    // must pass a real pointer.
    explicit ExportScriptable(Curvz::MainWindow* main_window);
    ~ExportScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // s251 m1 — RunContext mask declarations.
    //
    //   svg — ctx::Scripter | ctx::TestRunner. Macro OUT.
    //         Same mask shape as proj save (NOT proj save_as). See
    //         the verb-surface block above for the rationale: export
    //         produces a side artefact at a path, it does not
    //         rewrite the project's own identity; CANON's Scripter
    //         in-scope list explicitly permits /tmp writes for
    //         scratch and diagnostic dumps; the path-containment
    //         gate ($HOME + $TMPDIR) covers the system-integrity
    //         layer separately.
    //
    // NOTE: this is the SEVENTH consumer of context_mask() in the
    // codebase (Inspector + proj.{save, save_as, close, load, new} +
    // export.svg). The registry-promotion clock is now at 7/n; STILL
    // HELD by design. New rationale this session: export.svg's mask is
    // sibling to proj.save's (the Scripter | TestRunner pair), not a
    // novel shape — the catalogue still consists of exactly two
    // distinct mask values across seven verbs (Scripter | TestRunner
    // for save / svg; TestRunner-only for save_as / close / load /
    // new). Promotion-today's blast radius (every Scriptable's ctor
    // and dispatch path) still outweighs the benefit; the dedup
    // argument grows weaker as the catalogue gets MORE uniform, not
    // stronger. Hold until either (a) a typo bug bites, (b) the
    // catalogue grows past ~10 declarations, or (c) a novel mask
    // combination appears (Macro-only verb, TestRunner+special caller,
    // anything genuinely new). Banked observation.
    //
    // Future verbs in this Scriptable (`png`, `ico`, `refpt`,
    // `gresource`) will declare their own masks here as they land.
    // All five are expected to share svg's `Scripter | TestRunner`
    // shape because all five produce side artefacts with the same
    // trust profile; if a future format raises a distinct trust
    // concern (e.g. gresource's GResource bundles touch a different
    // layer than the others) that's the time to revisit per-verb.
    RunContextMask context_mask(std::string_view verb) const override;

private:
    Curvz::MainWindow* m_main_window;

    // s251 m1 — per-attempt observables for the smoke. These describe
    // the most recent svg / (future png / ico / refpt / gresource)
    // call, not project state. Both reset to empty / false at
    // construction; both update on EVERY successful export call (the
    // path supplied is captured to last_path, last_ok flips to true);
    // on a failed export, last_ok flips to false and last_path is
    // unchanged (so a script can tell which prior export's path it
    // still corresponds to).
    //
    // Choice: update on success only, not on attempt. Rationale: a
    // refused or failed export shouldn't blank the prior success's
    // path — the script may want to retry against a known-good
    // observable. If a use case ever names the "did the LAST
    // ATTEMPT (regardless of outcome) target this path" question, a
    // separate last_attempt_path member is easy to add without
    // breaking the existing query contract.
    std::string m_last_path;
    bool        m_last_ok = false;
};

} // namespace curvz::scripting
