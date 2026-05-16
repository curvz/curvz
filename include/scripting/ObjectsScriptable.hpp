// scripting/ObjectsScriptable.hpp ────────────────────────────────────────────
//
// s230 m1 — sixth row-bound model Scriptable, OPENING the multi-session
// `objects` arc. Wraps the active document's scene-content surface as
// a collection Scriptable under abbrev `objects`, materialising
// transient `objects.<iid>` proxies for per-instance read operations
// via the s216 m1 router hooks (Scriptable::can_resolve / proxy_for).
//
// This is the largest of the row-bound Scriptables in scope. The five
// shipped before this one (layers / guides / swatches / styles /
// themes) each wrapped a single collection — either a SceneNode-type-
// filter on the doc's top-level layers vector, or a project-scoped
// library. `objects` wraps "everything addressable in the scene tree"
// and walks recursively. Pattern stays the same as the five shipped
// (collection-as-router with transient per-instance proxies); the
// shape of the collection differs.
//
// ── Why "opening" — what m1 ships and what it doesn't ─────────────────────
//
// This is the foundation move: `objects.<iid>` becomes a resolvable
// address. From this milestone forward every script can REACH the
// scene; mutating it across later milestones is then incremental.
//
// What m1 ships (READ-SIDE only):
//   - The collection registered as `objects` in the registry.
//   - Collection queries: count, all_iids, find_by_name, find_by_type.
//   - Per-instance proxies materialised by proxy_for(iid).
//   - Element queries: name, type, visible, locked, parent_iid,
//     child_count, iid.
//   - NO mutating verbs anywhere — neither collection nor proxy.
//   - History pointer captured in the ctor signature (shape-symmetry
//     with the five existing model Scriptables; not dereferenced yet
//     because no verb pushes a command).
//
// What m1 explicitly does NOT ship:
//   - Element mutating verbs (toggle_visible, set_visible, set_locked,
//     rename). Scheduled m3+ — they ride EditObjectCommand /
//     EditNodeCommand surfaces that already exist for the inspector
//     and canvas drivers. The s167 iid migration means the command
//     stack is already keyed on CurvzProject* + obj_iid; no raw
//     SceneNode pointers in capture state.
//   - Structural verbs (move, reparent, delete, duplicate). Scheduled
//     m4+. These require careful interaction with the iid index (the
//     dirty bit, the s167 invariants).
//   - Collection-level structural verbs (new, group, ungroup).
//     Scheduled m5+. Open question for that milestone: which type to
//     create (the `new` verb here has to take a type argument),
//     which layer to add to (active? caller-specified?).
//   - selected_iids query. CANON places `selection` as a separate
//     singleton Scriptable, not a query on `objects`. Selection is
//     canvas-side state, not document-side state; mixing the two
//     here would couple this Scriptable to Canvas. Scheduled with
//     the `selection` singleton when that lands.
//
// Closing the whole arc may take 3-5 sessions. m1 is the seam-opening
// move; later milestones extend from this foundation.
//
// ── Scope of the collection ──────────────────────────────────────────────
//
// The s230 handoff surfaced three plausible boundaries for what
// `objects` should address:
//
//   (i) All SceneNodes including layers / guides / refs / measure
//       nodes. Most honest to the tree shape, but `objects.<iid>`
//       would collide with `layers.<iid>` and `guides.<iid>` for
//       the same iid — both addressing surfaces work but the
//       proxies differ. The router would need to handle precedence.
//
//   (ii) Only non-layer/guide/ref/measure SceneNodes — "real scene
//        contents only." Cleanest non-overlap with the four existing
//        Scriptables. Expandable to (i) later if scripts genuinely
//        need layer-typed nodes via `objects`.
//
//   (iii) Only the active layer's children. Mirrors how current
//         selection / tools think. Most restrictive; possibly too
//         restrictive — scripts that want to walk the whole document
//         can't.
//
// **m1 picks (ii).** Clean separation with `layers` / `guides`; the
// types `objects` covers are exactly those a user would call "scene
// contents." The doc tree walks every layer's children (not just the
// active layer's), so cross-layer scripts work without a layer-
// switching dance. If a future script genuinely needs to address a
// Layer node via `objects`, (i) is reachable from here — extend
// is_scene_object() in the .cpp; no API change.
//
// Types in scope for `objects`:
//   - Path             — vector-shape leaf, the most common case
//   - Group            — container of arbitrary scene children
//   - Compound         — boolean-result container (path-shaped output)
//   - ClipGroup        — clip-shape + clipped children
//   - Blend            — interpolation between two source nodes
//   - Warp             — envelope-deformed source
//   - Text             — text leaf
//   - Image            — raster leaf
//   - Ref              — reference-point marker (lives under RefLayer)
//   - Measurement      — measurement leaf (lives under MeasureLayer)
//
// Types explicitly OUT of scope (their own Scriptable, or no
// addressability at this tier):
//   - Layer            — owned by `layers`
//   - Guide            — owned by `guides`
//   - GuideLayer       — special structural container; owned by `guides`
//                        implicitly via guide_layer()
//   - RefLayer         — special structural container; would belong to
//                        a future `refs` Scriptable
//   - MeasureLayer     — special structural container; would belong
//                        to a future `measurements` Scriptable
//   - GridLayer        — special structural container; doc-level
//                        property, no per-row addressability
//   - MarginLayer      — special structural container; doc-level
//                        property, no per-row addressability
//
// The Ref / Measurement types ARE in scope despite their parents
// being special layers. The reasoning: a Ref or a Measurement is a
// leaf with its own iid, position, and per-instance properties; a
// script wanting to read those properties does so via `objects.<iid>`.
// Their parent containers (RefLayer / MeasureLayer) get their own
// future Scriptable surfaces; the leaves answer to `objects` because
// they're concrete scene leaves with stable identity.
//
// ── Addressing ───────────────────────────────────────────────────────────
//
//   objects                  — the collection Scriptable. Set queries
//                              (count, all_iids, find_by_name,
//                              find_by_type). ONE registry entry per
//                              app session; held as a member by
//                              MainWindow, registered for the lifetime
//                              of the window.
//
//   objects.<iid>            — per-instance proxy, materialised on
//                              demand by the collection. Reads the
//                              specific node's properties through the
//                              project's iid index. NOT registered —
//                              the registry only knows about `objects`.
//                              A `list` from the Scripter shows
//                              `objects` but never shows `objects.<iid>`
//                              entries (same property as the five
//                              shipped Scriptables).
//
// ── Lifetime and the project pointer ─────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable: a project getter
// (std::function<CurvzProject*()>) resolved fresh on every verb call,
// not a captured raw pointer. MainWindow's m_project unique_ptr gets
// reset and reassigned at runtime (close-project, load-project); the
// getter survives that, the Scriptable does not need re-registration.
//
// MainWindow constructs the ObjectsScriptable once (after m_project
// is initialised in setup_project) and the registry holds the entry
// for the window's lifetime.
//
// ── The proxy ────────────────────────────────────────────────────────────
//
// ObjectProxy is a private class inside ObjectsScriptable.cpp.
// Constructed by proxy_for(iid), returned as a unique_ptr<Scriptable>
// to the listener's ResolvedObject wrapper, destroyed when the wrapper
// goes out of scope (end of run_line). It holds:
//
//   - the project getter (shared with the parent ObjectsScriptable so
//     reads resolve through the current project)
//   - the iid (the per-instance key)
//   - the history pointer (captured but unused today, see "READ-SIDE
//     only" above — present for shape-symmetry with the five shipped
//     proxies, and to avoid a ctor signature change in m3+ when
//     mutating verbs land)
//
// Every proxy query body calls curvz::utils::find_by_iid(proj, iid)
// fresh — no caching of the live SceneNode pointer. If the iid no
// longer resolves (object deleted between dispatch lines), or if the
// resolved node is no longer of a `is_scene_object` type (rare edge
// case where a re-typed iid stops being a scene object), the proxy's
// queries return Null — matching the addressability-tracks-current-
// live-state invariant established for LayerProxy / GuideProxy.
//
// ── Verb / query surface ─────────────────────────────────────────────────
//
// COLLECTION VERBS (on `objects`):
//   (none in m1 — m3+ adds mutating verbs as commands land)
//
// COLLECTION QUERIES (on `objects`):
//   count                                — number of in-scope scene
//                                          objects in the active doc.
//                                          Walks every layer's
//                                          subtree, counts nodes
//                                          matching is_scene_object.
//                                          Cheap relative to typical
//                                          scene sizes; no caching.
//
//   all_iids                             — comma-separated iids in
//                                          tree-walk order (depth-
//                                          first, layer-top-down).
//                                          Same sentinel shape as
//                                          LayersScriptable /
//                                          GuidesScriptable all_iids
//                                          — waiting for the future
//                                          `foreach` listener-grammar
//                                          extension to consume it.
//                                          For the pilot the string
//                                          is a sentinel (presence
//                                          indicates the query works;
//                                          scripts can't iterate it
//                                          without grammar changes).
//
//   find_by_name "<name>"                — iid of the first node
//                                          whose `name` field matches
//                                          exactly, or "" on miss.
//                                          Walks every layer's
//                                          subtree depth-first.
//                                          Names aren't unique by
//                                          construction (dedup_names
//                                          handles load-time
//                                          collisions but live
//                                          collisions can happen);
//                                          "first hit in walk order"
//                                          is the contract — matches
//                                          LayersScriptable's
//                                          find_by_name semantics.
//
//   find_by_type "<type>"                — iid of the first node
//                                          whose type matches, or
//                                          "" on miss. Type tokens
//                                          are lowercase strings:
//                                          "path", "group",
//                                          "compound", "clipgroup",
//                                          "blend", "warp", "text",
//                                          "image", "ref",
//                                          "measurement". Walk order
//                                          and "first hit" same as
//                                          find_by_name. Useful for
//                                          smoke tests that need to
//                                          land an iid of a known
//                                          type without pre-knowing
//                                          one.
//
// ELEMENT QUERIES (on `objects.<iid>`):
//   name           — string       node.name. Empty if the user
//                                  hasn't named the node (the
//                                  default for newly-created paths
//                                  / shapes until renamed).
//   type           — string       lowercase type token (same
//                                  vocabulary as find_by_type:
//                                  "path", "group", "compound",
//                                  "clipgroup", "blend", "warp",
//                                  "text", "image", "ref",
//                                  "measurement"). Stable across
//                                  the Scriptable's lifetime — the
//                                  string vocabulary is part of the
//                                  contract.
//   visible        — bool         node.visible
//   locked         — bool         node.locked
//   parent_iid     — string       internal_id of the node's parent
//                                  container (Layer / Group /
//                                  Compound / ClipGroup / Blend /
//                                  Warp / RefLayer / MeasureLayer).
//                                  The parent IS the owning slot;
//                                  for a Path that lives directly
//                                  under a Layer, parent_iid is the
//                                  Layer's iid. For a Path inside a
//                                  Group inside a Layer,
//                                  parent_iid is the Group's iid.
//                                  Returns "" if the parent walk
//                                  doesn't find the node (shouldn't
//                                  happen for a node that resolved
//                                  through find_by_iid — defensive).
//   child_count    — int          node.children.size(). Always
//                                  defined; 0 for leaf types (Path,
//                                  Text, Image, Ref, Measurement)
//                                  in practice, but the field
//                                  reads from the live vector so
//                                  even if someone abuses a leaf
//                                  with children it still answers.
//   iid            — string       node.internal_id (for completeness:
//                                  a script can `get objects.<iid>
//                                  iid` to verify it has the right
//                                  handle, useful in tests).
//
// ELEMENT VERBS (on `objects.<iid>`):
//   (none in m1 — m3+ adds toggle_visible / set_visible / set_locked
//   / rename as EditObjectCommand-pushing verbs)
//
// ── On the active-document scope ─────────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable: collection
// queries operate on project->active_doc(); element queries resolve
// through curvz::utils::find_by_iid(project, iid), which walks every
// document. So `objects count` counts the ACTIVE doc, but `get
// objects.<iid> name` finds the iid in WHICHEVER doc holds it. The
// iid is the address; the doc is just where it lives.
//
// ── Why no rename and no find_by_iid query ──────────────────────────────
//
// Rename: that's a mutating verb (m3+). Reading via `get objects.<iid>
// name` covers the read side.
//
// find_by_iid: not a query — it's the resolver itself. A script that
// has an iid in hand goes straight to `objects.<iid>` addressing; no
// query needed. `find_by_name` and `find_by_type` exist because
// scripts often want to start from a property (a known name, a
// known type) and bind an iid; `find_by_iid` would be a no-op
// (iid in, iid out).
//
// ── Diagnostic-only ──────────────────────────────────────────────────────
//
// This header is included only from .cpp files compiled in the
// CURVZ_DIAGNOSTIC build; production builds don't see it. MainWindow's
// ObjectsScriptable member is held behind the same scripting-TU
// compilation gate as the five sibling members (always-compiled as of
// s219 m1).

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

class ObjectsScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;

    // Registers as "objects" via the Scriptable base ctor. The project
    // getter is called fresh on every verb invocation — never cached —
    // so MainWindow can swap its m_project unique_ptr freely (open
    // another project, close project) without invalidating us. The
    // getter is allowed to return nullptr; queries degrade gracefully
    // (count returns 0, all_iids returns "", find_by_* returns "").
    //
    // `history` is non-owning and may be nullptr. Captured but unused
    // in m1 — no mutating verbs yet, so no commands get pushed. Present
    // in the ctor signature so the day m3+ adds mutating verbs the
    // wiring is already in place; same shape-symmetry argument used
    // for GuidesScriptable's pre-undoability history pointer.
    ObjectsScriptable(ProjectGetter get_project,
                      Curvz::CommandHistory* history);
    ~ObjectsScriptable() override = default;

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
                                        // unused in m1 (read-side only)
};

} // namespace curvz::scripting
