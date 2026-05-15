// scripting/GuidesScriptable.hpp ─────────────────────────────────────────────
//
// s218 m1 — second concrete row-bound model Scriptable, following the
// LayersScriptable shape banked in s216 m1 + s217 m2. Wraps the active
// document's guide surface as a collection Scriptable under abbrev
// `guides`, materialising transient `guides.<iid>` proxies for per-
// instance operations via the s216 m1 router hooks
// (Scriptable::can_resolve / proxy_for).
//
// ── How guides live in the model ────────────────────────────────────────────
//
// A guide is a SceneNode with Type::Guide, parked as a child of the
// document's GuideLayer (a top-of-stack special layer ensured by
// CurvzDocument::ensure_guide_layer). The three fields that define a
// guide are guide_x / guide_y / guide_angle (degrees, 0=horizontal,
// 90=vertical, arbitrary between). The structural prerequisite the
// s217 handoff flagged — "does Guide have internal_id?" — is satisfied
// by s168 m2's default-init of SceneNode::internal_id; every guide
// entering the tree has an iid by construction.
//
// Guides are NOT undoable today. The PropertiesPanel guide section
// (build_object_guides_section) mutates guide fields directly without
// pushing commands; ruler-drag and "From 2 points…" both push children
// onto the guide layer without command participation. This Scriptable
// mirrors that posture exactly — direct mutation on every verb, no
// EditXxxFieldCommand equivalent for guides exists. The CommandHistory*
// parameter is kept in the ctor signature for shape-symmetry with
// LayersScriptable and so the day guides become undoable (whenever
// someone adds a Guide field-edit command) the wiring is already in
// place; for now the pointer is captured-but-unused.
//
// ── Addressing ──────────────────────────────────────────────────────────────
//
//   guides                  — the collection Scriptable. Set verbs and
//                             set queries plus the doc-level guide
//                             colour. ONE registry entry per app
//                             session; held as a member by MainWindow,
//                             registered for the lifetime of the window.
//
//   guides.<iid>             — per-instance proxy, materialised on
//                             demand by the collection. Reads / writes
//                             the specific guide's fields through the
//                             project's iid index. NOT registered —
//                             the registry only knows about `guides`.
//                             A `list` from the Scripter shows `guides`
//                             but never shows `guides.<iid>` entries.
//
// ── Lifetime and the project pointer ────────────────────────────────────────
//
// Same shape as LayersScriptable: a project getter
// (std::function<CurvzProject*()>) resolved fresh on every verb call,
// not a captured raw pointer. MainWindow's m_project unique_ptr gets
// reset and reassigned at runtime (close-project, load-project); the
// getter survives that, the Scriptable does not need re-registration.
//
// MainWindow constructs the GuidesScriptable once (after m_project is
// initialised in setup_project) and the registry holds the entry for
// the window's lifetime.
//
// ── The proxy ───────────────────────────────────────────────────────────────
//
// GuideProxy is a private class inside GuidesScriptable.cpp. Constructed
// by proxy_for(iid), returned as a unique_ptr<Scriptable> to the
// listener's ResolvedObject wrapper, destroyed when the wrapper goes
// out of scope (end of run_line). It holds:
//
//   - the project getter (shared with the parent GuidesScriptable so
//     mutations resolve through the current project)
//   - the iid (the per-instance key)
//   - the history pointer (unused today, see header rationale above;
//     present for shape-symmetry with LayerProxy)
//
// Every proxy verb/query body calls curvz::utils::find_by_iid(proj, iid)
// fresh — no caching of the live SceneNode pointer. If the iid no longer
// resolves (guide deleted between dispatch lines) the proxy's invoke
// returns Null and query returns Null too, matching the addressability-
// tracks-current-live-state invariant established for LayerProxy.
//
// ── Verb / query surface ────────────────────────────────────────────────────
//
// The verb vocabulary is the PropertiesPanel guide-section vocabulary,
// per the s216 / s217 banked rule "verb names come from the
// application's own UI vocabulary, not API conventions". The guide
// section is smaller than the layer row, so the surface here is
// correspondingly smaller — no name (guides have none in the UI), no
// per-guide visibility (visibility lives on the whole GuideLayer, not
// on individual guides), no rename.
//
// COLLECTION VERBS (on `guides`):
//   new                                  — create a new guide at the
//                                          centre of the active doc's
//                                          canvas, angle 0 (horizontal).
//                                          Subsequent x/y/angle proxy
//                                          verbs shape it. Returns iid.
//                                          Mirrors the model side of
//                                          Canvas::begin_guide_drag's
//                                          insertion — push a Type::Guide
//                                          node into doc->guide_layer()
//                                          ->children, no command push
//                                          (guides aren't undoable).
//   delete "<iid>"                       — remove the guide from its
//                                          parent guide layer. Walks
//                                          the project to find the doc
//                                          that holds the iid (same
//                                          cross-doc-iid story as
//                                          LayersScriptable's delete).
//   color "<hex>"                        — write the active doc's
//                                          guide_color_r/g/b from a hex
//                                          string. Empty string clears
//                                          to the default cyan. This is
//                                          a DOC-LEVEL property (all
//                                          guides share one colour),
//                                          not per-guide — matches the
//                                          inspector's Color row, which
//                                          edits doc->guide_color_*.
//                                          No undo (the inspector
//                                          doesn't push for this edit
//                                          either).
//
// COLLECTION QUERIES (on `guides`):
//   count                                — number of Type::Guide nodes
//                                          in the active doc's guide
//                                          layer
//   all_iids                             — comma-separated iids in
//                                          children-order. Same sentinel
//                                          shape as LayersScriptable's
//                                          all_iids — waiting for the
//                                          future `foreach` grammar.
//   color                                — the active doc's guide_color
//                                          as a hex string (e.g.
//                                          "#00bfff" for the default)
//
// ELEMENT VERBS (on `guides.<iid>`):
//   toggle_locked                        — flip locked (direct write)
//   set_locked <bool>                    — write locked (direct write)
//   x <value>                            — write guide_x (direct write)
//   y <value>                            — write guide_y (direct write)
//   angle <value>                        — write guide_angle (direct
//                                          write). Degrees, same
//                                          convention as the model:
//                                          0=horizontal, 90=vertical.
//
// ELEMENT QUERIES (on `guides.<iid>`):
//   locked         — bool         guide.locked
//   x              — double       guide.guide_x
//   y              — double       guide.guide_y
//   angle          — double       guide.guide_angle
//   type           — string       "h" | "v" | "a" — matches the type
//                                  label in build_object_guides_section
//                                  (case lowered from the panel's H/V/A
//                                  for script-stable lookups)
//   iid            — string       guide.internal_id (for completeness,
//                                  same as LayerProxy::iid)
//
// ── On the active-document scope ────────────────────────────────────────────
//
// Same shape as LayersScriptable: collection verbs operate on
// project->active_doc(); element verbs resolve through
// curvz::utils::find_by_iid(project, iid), which walks every document.
// So `guides count` counts the ACTIVE doc, `guides new` adds to the
// ACTIVE doc, but `guides.<iid> angle 45` finds the iid in WHICHEVER
// doc holds it.
//
// ── Why no rename / no find_by_name ─────────────────────────────────────────
//
// Guides have no `name` field used anywhere in the UI. The inspector
// labels each guide by type ("H", "V", "A") which is derived from
// guide_angle, not stored. Scripts addressing guides do so by iid
// (returned from `new`, enumerable via `all_iids`); there's no name
// surface to look up by.
//
// ── Diagnostic-only ─────────────────────────────────────────────────────────
//
// This header is included only from .cpp files compiled in the
// CURVZ_DIAGNOSTIC build; production builds don't see it. MainWindow's
// GuidesScriptable member is held behind #ifdef CURVZ_DIAGNOSTIC.

#pragma once
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptValue.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Curvz {
struct CurvzProject;
class  CommandHistory;
}

namespace curvz::scripting {

class GuidesScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;

    // Registers as "guides" via the Scriptable base ctor. The project
    // getter is called fresh on every verb invocation — never cached —
    // so MainWindow can swap its m_project unique_ptr freely. The
    // getter is allowed to return nullptr; verbs degrade gracefully
    // (count returns 0, all_iids returns "", etc.).
    //
    // `history` is non-owning and may be nullptr. Captured but unused
    // today — guides aren't undoable. Present for shape-symmetry with
    // LayersScriptable and to avoid a ctor signature change the day a
    // Guide-flavored field-edit command lands.
    GuidesScriptable(ProjectGetter get_project,
                     Curvz::CommandHistory* history);
    ~GuidesScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // Router hooks — see CANON's "Row-bound model Scriptables".
    bool can_resolve(std::string_view key) const override;
    std::unique_ptr<Scriptable> proxy_for(std::string_view key) override;

private:
    ProjectGetter          m_get_project;
    Curvz::CommandHistory* m_history;   // non-owning; captured but
                                        // unused today (see header)
};

} // namespace curvz::scripting
