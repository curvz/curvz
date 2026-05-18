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
// ── Verb surface (s251 m1: `svg`; s252 m2: + `png`; s253 m1: + `theme`) ────
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
//   png <path> <size>       — write the project's ACTIVE DOCUMENT as
//                             a PNG file at the supplied path. SECOND
//                             format-verb on this Scriptable (s252 m2;
//                             m5b export-surface arc 2/5). First
//                             two-arg verb on this Scriptable; first
//                             numeric-arg verb on any m5b headless-
//                             verb singleton.
//
//                             `<size>` is the LONGEST-SIDE pixel
//                             count. Width/height are computed from
//                             the active doc's aspect ratio inside
//                             MainWindow's helper, mirroring
//                             ExportDialog.cpp's fit_width / fit_height
//                             calculation (the GUI's Export Documents
//                             dialog also collects ONE size value and
//                             derives the other dimension from the
//                             document's aspect ratio — same mental
//                             model, same arithmetic).
//
//                             Choice of longest-side over a two-int
//                             (width × height) signature: matches the
//                             GUI's single-size-field UI; matches
//                             CANON's "headless-verb singletons"
//                             frame (script supplies the same
//                             ARGUMENT the picker collects, and the
//                             picker collects ONE size value); keeps
//                             the verb signature simple for the m2
//                             ship's open-the-shape job. If a use
//                             case ever names "I need a specific
//                             non-aspect-preserving width × height",
//                             a sibling verb `png_sized <path> <w> <h>`
//                             can graduate later without breaking
//                             this one.
//
//                             Contract — six refusal paths and one
//                             happy path. Same five refusals as svg
//                             plus one new size-validation refusal:
//
//                             - Wrong argument shape (not exactly two
//                               args; first arg not coercible to
//                               string; second arg not coercible to
//                               int): refuse with structured error.
//                               Argument validation fires at the
//                               Scriptable layer before any state
//                               inspection.
//
//                             - Empty path (first arg coerces to ""):
//                               refuse. Same shape as svg's empty-
//                               path refusal.
//
//                             - Invalid size (size <= 0): refuse with
//                               structured error "size must be a
//                               positive integer". Mirrors the
//                               stricter-than-GUI posture that landed
//                               on save_as / load / new (the GUI's
//                               SpinButton already enforces > 0; the
//                               script side matches by refusing
//                               rather than clamping). Upper-bound
//                               policy intentionally left open — the
//                               writer probably handles 100000 fine,
//                               and an upper bound is a "use case
//                               names the need" item.
//
//                               IMPORTANT: size-invalid is an
//                               ARGUMENT-SHAPE refusal (same layer as
//                               the path-empty / wrong-arg-count
//                               guards), not an outcome of the
//                               MainWindow helper. It throws
//                               directly from the Scriptable without
//                               going through an enum branch, same as
//                               svg's path-empty refusal does. Layering
//                               principle: argument shape lives in
//                               the Scriptable, path containment lives
//                               in path_is_safe, project-state and
//                               I/O live in the helper. Each layer
//                               speaks its own concern; SizeInvalid
//                               belongs at the argument-shape layer.
//
//                             - Path-containment refusal: refuse with
//                               path_is_safe's reason string. FIFTH
//                               production call site (after save_as
//                               s247 m1, load s249 m1, new s250 m1,
//                               export svg s251 m1); SECOND
//                               cross-Scriptable application after
//                               svg.
//
//                             - No project loaded: refuse.
//
//                             - No active document: refuse.
//
//                             - I/O failure (export_png_sized
//                               returned false): refuse with
//                               structured error. Mirrors svg's
//                               IoFailed.
//
//                             - Otherwise: compute width/height from
//                               size + doc aspect ratio, call
//                               export_png_sized(doc, path, w, h),
//                               update last_path / last_ok. Return
//                               Null (listener prints `ok`).
//
//                             RunContext mask:
//                                 ctx::Scripter | ctx::TestRunner
//                             — SAME mask shape as svg (and proj
//                             save). PNG is also a side-artefact
//                             writer; same trust profile as svg
//                             warrants the same mask. The
//                             path-containment gate and Macro
//                             exclusion cover the system-integrity
//                             and recorded-automation layers
//                             respectively.
//
//   theme <dir>             — write the project's documents as a
//                             freedesktop icon-theme bundle at
//                             `<dir>`. THIRD format-verb on this
//                             Scriptable (s253 m1; m5b export-
//                             surface arc 3/5). The first verb in
//                             m5b whose path argument names a
//                             DIRECTORY rather than a file — the
//                             output is a tree of subdirectories
//                             (scalable/<category>/*.svg plus
//                             16x16/24x24/32x32/48x48/64x64/128x128/
//                             256x256 PNG bundles per category)
//                             plus three metadata files
//                             (gresource.xml, index.theme, README.md).
//
//                             Why this is called `theme`, not `ico`:
//                             the bundle is a FREEDESKTOP ICON THEME
//                             (a directory tree consumable by GTK
//                             apps and system-wide hicolor
//                             installations), not a Windows `.ico`
//                             file. The output structure exactly
//                             mirrors what the GUI's File ▸ Export…
//                             ▸ Icon Theme tab produces. The verb
//                             name is the same noun the GUI uses
//                             ("Icon Theme") plus the s251 m1
//                             one-word convention; "ico" would
//                             have read as Windows .ico and
//                             misled future readers about what
//                             this verb produces. (A genuine
//                             Windows .ico writer can graduate
//                             later as a sibling verb `winico`
//                             without re-litigating this naming
//                             call.)
//
//                             Argument shape: ONE arg (the output
//                             directory). All other GUI-collected
//                             parameters get sensible defaults:
//
//                             - Entry list: every document in the
//                               project's `documents` vector with
//                               include=true. The GUI's checklist
//                               UI is replaced by "include
//                               everything"; if a use case ever
//                               names "include only some docs",
//                               a sibling verb or arg extension
//                               can graduate.
//                             - theme_name: project basename
//                               derived from m_project->directory
//                               (e.g. /home/me/myapp.curvz →
//                               "myapp"). Fallback "MyIcons" if
//                               the basename is empty (rare;
//                               only when the project has never
//                               been saved, in which case the
//                               NoPath refusal fires first).
//                             - theme_comment: empty string. The
//                               GUI's "Icons for MyApp" pattern
//                               is also defaultable to empty;
//                               the field is informational in
//                               the generated index.theme.
//
//                             Contract — five refusal paths and
//                             one happy path. Same five refusals
//                             as svg (no size arg → no size-
//                             invalid refusal):
//
//                             - Wrong argument shape (not exactly
//                               one arg, or arg not coercible to
//                               non-empty string): refuse with
//                               structured error. Same posture as
//                               svg's empty-path and arg-count
//                               refusals; messages mirror svg's.
//
//                             - Path-containment refusal: refuse
//                               with path_is_safe's reason string.
//                               SIXTH production call site (after
//                               save_as s247 m1, load s249 m1,
//                               new s250 m1, svg s251 m1, png
//                               s252 m2); THIRD cross-Scriptable
//                               application on `export`. The
//                               `<dir>` is treated by path_is_safe
//                               identically to a file path —
//                               realpath() walks up to the
//                               nearest existing ancestor, then
//                               the containment check fires
//                               against $HOME / $TMPDIR. Whether
//                               `<dir>` already exists or not
//                               doesn't change the gate's
//                               semantics; ensure_dir() inside
//                               the helper creates the directory
//                               on the happy path.
//
//                             - No project loaded: refuse.
//
//                             - No project path (m_project loaded
//                               but its directory is empty —
//                               theme_name derivation would have
//                               no source). Refuse with structured
//                               error pointing the script author
//                               at proj save_as. This refusal
//                               doesn't have an analogue in svg /
//                               png because those verbs don't
//                               read m_project->directory; theme
//                               does, to derive theme_name. A new
//                               enum branch `NoProjectPath` on the
//                               result enum names the outcome.
//
//                             - No active document state isn't a
//                               refusal here — `theme` writes
//                               EVERY document in the project,
//                               not the active one. If the project
//                               is empty (zero documents), the
//                               helper's pre-check sees no valid
//                               entries and the writer returns
//                               IoFailed with the "No icons to
//                               export" message. Same semantic
//                               surface as the GUI's Icon Theme
//                               tab: the user sees the same status
//                               message ("No icons to export.
//                               Assign a name and category to each
//                               icon in the inspector.") if no
//                               valid entries materialise. The
//                               script-side observable is the
//                               refusal under IoFailed.
//
//                             - I/O failure (export_theme returned
//                               with success=false — couldn't
//                               create output_dir, or no valid
//                               icons after filtering, or every
//                               PNG/SVG write failed). Refuse with
//                               structured error including the
//                               helper's error_message if any. The
//                               helper's success criterion is
//                               icons_written > 0; sub-document
//                               failures are logged but don't
//                               propagate unless EVERY document
//                               fails. This matches the GUI's
//                               behaviour exactly.
//
//                             - Otherwise: write the bundle, return
//                               Null. last_path captures the
//                               directory (not a file path —
//                               cross-format last_path holds
//                               whatever path argument the most
//                               recent successful export was
//                               given, which for `theme` is the
//                               output directory). last_ok flips
//                               to true.
//
//                             RunContext mask:
//                                 ctx::Scripter | ctx::TestRunner
//                             — SAME mask shape as svg and png.
//                             theme is also a side-artefact writer
//                             with the same trust profile: it
//                             writes a tree of files at the
//                             supplied path, but does not modify
//                             the loaded project's identity. The
//                             path-containment gate covers system
//                             integrity (the directory must be
//                             under $HOME or $TMPDIR); Macro
//                             exclusion covers recorded-automation
//                             surface; the user-data integrity
//                             layer is preserved because the
//                             project itself is unchanged.
//
//                             One nuance worth noting: theme
//                             writes MORE FILES than svg/png
//                             (potentially dozens — one SVG + 7
//                             PNGs per document per category). A
//                             malicious script with Scripter
//                             permission could write a lot of
//                             bytes inside $HOME/$TMPDIR in a
//                             single call. This doesn't change
//                             the trust calculus — the path
//                             containment is the system-integrity
//                             gate, and the volume of files is
//                             a quantitative difference, not a
//                             qualitative one — but it's worth
//                             noting for future audit-log review:
//                             a theme export logs more lines than
//                             an svg export, and the audit story
//                             across format-verbs is not uniform
//                             in line count.
//
// ── Query surface (s251 m1: `last_path`, `last_ok`; s252 m2: shared with png; s253 m1: shared with theme) ─
//
//   last_path  — text. Path of the most recent SUCCESSFUL export. Empty
//                string if no export has succeeded yet this session
//                (initial state) or if every attempted export failed.
//                Read-only.
//
//                s252 m2 — last_path is SHARED ACROSS ALL FORMAT-VERBS.
//                A successful `export svg "/tmp/a.svg"` followed by a
//                successful `export png "/tmp/b.png" 256` leaves
//                last_path == "/tmp/b.png". This is the right shape:
//                the observable answers "what was the last successful
//                export, regardless of format" — a script that
//                inspects last_path after multiple format calls cares
//                about the most recent success, not the most recent
//                per-format success. Per-format observables can
//                graduate later (last_svg_path / last_png_path / etc)
//                if a use case names the need; today's cross-format
//                sharing is the simpler shape.
//
//                s253 m1 — theme widens this from "file path" to
//                "path argument". The directory `<dir>` supplied to
//                `export theme <dir>` lands in last_path on success
//                unchanged; a script sees it as a normal string. No
//                special "this is a directory" marker — the
//                observable is the argument the verb received, and
//                the verb's documentation tells the script what
//                kind of artefact lives at that path. If a future
//                use case names "I need to know whether the last
//                export was a file or directory", a sibling query
//                (last_format, returning "svg" / "png" / "theme")
//                is the natural shape; today's cross-format sharing
//                stays simple.
//
//   last_ok    — boolean. true iff the most recent export call
//                succeeded. false at session start (no exports yet) and
//                after any failed export. Read-only. Also SHARED ACROSS
//                ALL FORMAT-VERBS — same rationale as last_path.
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
    // s252 m2 — png declaration added.
    // s253 m1 — theme declaration added.
    //
    //   svg   — ctx::Scripter | ctx::TestRunner. Macro OUT.
    //           Same mask shape as proj save (NOT proj save_as). See
    //           the verb-surface block above for the rationale:
    //           export produces a side artefact at a path, it does
    //           not rewrite the project's own identity; CANON's
    //           Scripter in-scope list explicitly permits /tmp
    //           writes for scratch and diagnostic dumps; the
    //           path-containment gate ($HOME + $TMPDIR) covers the
    //           system-integrity layer separately.
    //
    //   png   — ctx::Scripter | ctx::TestRunner. Macro OUT.
    //           SAME mask shape as svg. PNG is a side-artefact
    //           writer with the same trust profile — the path is
    //           the only disk surface touched, the project itself
    //           is unchanged, the path-containment gate covers
    //           system integrity, and Macro exclusion covers
    //           recorded-automation surface. Predicted in s251
    //           m1's "future verbs all expected to share svg's
    //           mask shape" note; confirmed at s252 m2.
    //
    //   theme — ctx::Scripter | ctx::TestRunner. Macro OUT.
    //           SAME mask shape as svg and png. Theme is also a
    //           side-artefact writer — it writes a TREE of files
    //           under <dir>, but the loaded project's own
    //           identity is unchanged. The trust profile is
    //           identical to svg/png modulo file count; see the
    //           theme verb-surface design block for the
    //           file-count nuance and why it doesn't change the
    //           mask call. Third instance of the
    //           "future format-verbs all share svg's mask" prediction;
    //           the prediction is now confirmed across three
    //           formats. If a future format raises a distinct
    //           trust concern (gresource is the most likely
    //           candidate — its bundles touch a different
    //           consumer layer), that's the time to revisit per-
    //           verb. For theme, the rationale lines up cleanly
    //           with svg/png.
    //
    // NOTE: this is the NINTH consumer of context_mask() in the
    // codebase (Inspector + proj.{save, save_as, close, load, new} +
    // export.{svg, png, theme}). The registry-promotion clock is
    // now at 9/n; STILL HELD by design. Catalogue: TWO distinct
    // mask values across nine verbs (Scripter | TestRunner for
    // save / svg / png / theme; TestRunner-only for save_as /
    // close / load / new). The ratio is GETTING MORE UNIFORM,
    // not less — seven of nine verbs now share one mask shape.
    // The dedup argument that would drive a central register_verb
    // table weakens further with each sibling-shape addition; the
    // strict trigger conditions stay: (a) a typo bug bites,
    // (b) catalogue grows past ~10 declarations, or (c) a novel
    // mask combination appears.
    //
    // The catalogue is now ONE verb away from the "past ~10"
    // promotion trigger. The next m5b verb (`refpt` or
    // `gresource`) will land that threshold; promotion to
    // central register_verb registry becomes a real conversation
    // at that point. Banked observation; clock at 9/n.
    //
    // Future verbs in this Scriptable (`refpt`, `gresource`)
    // will declare their own masks here as they land. Both are
    // expected to share svg/png/theme's `Scripter | TestRunner`
    // shape because all produce side artefacts with the same
    // trust profile; gresource's GResource bundle (a .gresource
    // binary readable by any GTK app) is the most likely
    // candidate to raise a distinct trust concern if any does.
    RunContextMask context_mask(std::string_view verb) const override;

private:
    Curvz::MainWindow* m_main_window;

    // s251 m1 — per-attempt observables for the smoke. These describe
    // the most recent svg / png / theme / (future refpt / gresource)
    // call, not project state. SHARED ACROSS ALL FORMAT-VERBS — any
    // successful export updates these, regardless of format; any
    // failed export leaves m_last_path untouched and m_last_ok set
    // to false. See the query-surface design block for the
    // cross-format-sharing rationale.
    //
    // Both reset to empty / false at construction; both update on
    // EVERY successful export call (the path supplied is captured
    // to last_path, last_ok flips to true); on a failed export,
    // last_ok flips to false and last_path is unchanged (so a
    // script can tell which prior export's path it still
    // corresponds to).
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
