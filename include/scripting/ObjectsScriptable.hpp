// scripting/ObjectsScriptable.hpp ────────────────────────────────────────────
//
// s230 m1 — sixth row-bound model Scriptable, OPENING the multi-session
// `objects` arc. Wraps the active document's scene-content surface as
// a collection Scriptable under abbrev `objects`, materialising
// transient `objects.<iid>` proxies for per-instance read operations
// via the s216 m1 router hooks (Scriptable::can_resolve / proxy_for).
//
// s231 m2 — adds two collection-level structural verbs (`new <type>`
// and `delete <iid>`) so the smoke can build up its own test objects
// and tear them back down — the same owns-its-conditions BUILD UP /
// VALIDATE / TEAR DOWN structure 33_layers / 27_guides established.
// Element queries from m1 are now exercised end-to-end on iids the
// smoke itself minted, not on whatever happened to be in the doc at
// test time.
//
// s232 m3 — adds five element mutating verbs on the proxy:
// `toggle_visible`, `set_visible <bool>`, `toggle_locked`,
// `set_locked <bool>`, `rename "<name>"`. Each is a thin shell over
// the new `EditObjectFieldCommand` (a direct analog of
// `EditLayerFieldCommand` minus the Color field). The s230 m1
// header's prediction was that these verbs could ride existing
// `EditObjectCommand` / `EditNodeCommand` surfaces — that turned
// out to be wrong. `EditObjectCommand` is the inspector's
// appearance/path-edit command (path / fill / stroke / shadow);
// it doesn't carry name / visible / locked. m3 adds
// `EditObjectFieldCommand` to close the gap. The existing user-
// facing rename path in LayersPanel for non-layer objects has
// been DIRECT mutation with no undo support since forever; m3
// keeps the change scoped to the script path, leaving the panel
// sweep as future band-3 mop-up.
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
// ── Arc progress (multi-session by construction) ──────────────────────────
//
// What m1 + m2 + m3 ship combined:
//   - The collection registered as `objects` in the registry.
//   - Collection queries: count, all_iids, find_by_name, find_by_type.
//   - Per-instance proxies materialised by proxy_for(iid).
//   - Element queries: name, type, visible, locked, parent_iid,
//     child_count, iid.
//   - Collection-level structural verbs: new <type>, delete <iid>.
//   - Element mutating verbs: toggle_visible, set_visible <bool>,
//     toggle_locked, set_locked <bool>, rename "<name>".
//   - History pointer captured in the ctor and ACTIVELY USED by the
//     structural verbs (each pushes AddNodeCommand / DeleteObjectCommand)
//     AND by the element mutating verbs (each pushes
//     EditObjectFieldCommand). All m3 verbs degrade gracefully to
//     direct mutation when history is null.
//
// What m3 still does NOT ship:
//   - Element structural verbs (move, reparent, duplicate). Scheduled
//     m4. These require careful interaction with the iid index (the
//     dirty bit, the s167 invariants) — the same "command pushes
//     direct mutation then invalidate" shape m2 uses for `new` /
//     `delete`, but with the parent-walk wired through too.
//   - Collection-level grouping verbs (group, ungroup). Scheduled m5.
//   - selected_iids query. CANON places `selection` as a separate
//     singleton Scriptable, not a query on `objects`. Selection is
//     canvas-side state, not document-side state; mixing the two
//     here would couple this Scriptable to Canvas. Scheduled with
//     the `selection` singleton when that lands.
//
// Closing the whole arc may take 3-5 sessions total. m1 opened the
// addressing surface; m2 makes it self-contained for testing; m3
// adds the element-level mutating surface; m4 adds element-structural
// verbs; m5 closes with grouping.
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
//                              find_by_type) plus the structural
//                              verbs `new` / `delete`. ONE registry
//                              entry per app session; held as a
//                              member by MainWindow, registered for
//                              the lifetime of the window.
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
// ObjectProxy is the per-instance Scriptable materialised by
// proxy_for(iid). Same lifetime contract as LayerProxy / GuideProxy:
// transient, unregistered, owned by the listener's ResolvedObject
// wrapper for the duration of one statement. Holds the iid as the
// stable handle; resolves through curvz::utils::find_by_iid every
// query (no cached SceneNode pointer — that would dangle if the
// underlying iid index rebuilds or the node moves).
//
// COLLECTION QUERIES (on `objects`):
//   count          — int          number of in-scope scene objects
//                                  in the active document, summed
//                                  recursively across all layers.
//   all_iids       — string       comma-separated iid list in walk
//                                  order. Empty string if count == 0.
//                                  No trailing comma. Walk order is
//                                  depth-first within each top-level
//                                  layer, layers in their natural
//                                  vector order.
//
// COLLECTION VERBS (on `objects`):
//   new "<type>"                 — Creates a new in-scope scene object
//                                  of the given type and inserts it
//                                  at the front of the active layer's
//                                  children. Returns the new node's
//                                  iid as a string (binds to `result`,
//                                  consumable by `set <var> to result`
//                                  in scripts). Pushes an
//                                  AddNodeCommand so Ctrl+Z removes
//                                  the new node.
//
//                                  Type vocabulary in m2:
//                                    "path"  — Path leaf with empty
//                                              PathData (no nodes,
//                                              not closed). Renders
//                                              as nothing; addressable
//                                              for property queries.
//                                    "group" — Group container with
//                                              no children. Renders
//                                              as nothing; addressable.
//
//                                  Returns "" on miss (no active doc,
//                                  no active layer, unknown type
//                                  token, args.empty()).
//
//                                  Name policy: NEW NODES ARE MINTED
//                                  WITH AN EMPTY NAME. Different from
//                                  canvas-tool creation which assigns
//                                  next_default_name (`"Path 1"` etc.).
//                                  Empty name is the "user hasn't
//                                  named me yet" sentinel — `assert
//                                  objects.<iid> name == ""` is the
//                                  deterministic post-create check.
//                                  m3's `rename` verb is how a script
//                                  assigns a name.
//
//                                  Insertion: front of the active
//                                  layer's children vector. Matches
//                                  the canvas convention (most tools
//                                  insert-at-front so the new node
//                                  draws on top).
//
//   delete "<iid>"               — Removes the in-scope scene object
//                                  identified by iid from its parent
//                                  container, wherever in the tree
//                                  that container lives. Pushes a
//                                  DeleteObjectCommand so Ctrl+Z
//                                  restores the node at its original
//                                  index.
//
//                                  Returns null (no result binding).
//                                  Returns null on miss (no project,
//                                  no active doc, args.empty(), iid
//                                  doesn't resolve to an in-scope
//                                  scene object, the resolved node
//                                  has no findable parent container).
//
//                                  Scrubs the global undo stack for
//                                  raw-pointer-capture references to
//                                  the about-to-be-deleted node and
//                                  its descendants — same defensive
//                                  walk LayersScriptable::delete does.
//
//   find_by_name "<name>"        — iid of the first node in the doc
//                                  tree with name matching exactly,
//                                  or "" on miss. Names aren't unique
//                                  by construction (the document name
//                                  uniquifier on load only normalises
//                                  load-time collisions but live
//                                  collisions can happen);
//                                  "first hit in walk order" is the
//                                  contract — matches
//                                  LayersScriptable's
//                                  find_by_name semantics.
//
//   find_by_type "<type>"        — iid of the first node whose type
//                                  matches, or "" on miss. Type
//                                  tokens are lowercase strings:
//                                  "path", "group", "compound",
//                                  "clipgroup", "blend", "warp",
//                                  "text", "image", "ref",
//                                  "measurement". Walk order and
//                                  "first hit" same as find_by_name.
//                                  Useful for smoke tests that need
//                                  to land an iid of a known type
//                                  without pre-knowing one.
//
// ELEMENT QUERIES (on `objects.<iid>`):
//   name           — string       node.name. Empty for newly-created
//                                  nodes (the contract above); user-
//                                  named once a m3+ `rename` verb
//                                  runs.
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
//   toggle_visible                 — flip visible (push EditObjectFieldCommand).
//                                    No-args.
//   set_visible <bool>             — write visible to the given bool.
//                                    No-op (no command pushed) when the
//                                    new value equals the current one —
//                                    same behaviour as LayerProxy.
//   toggle_locked                  — flip locked (push EditObjectFieldCommand).
//   set_locked  <bool>             — write locked to the given bool.
//                                    No-op when value unchanged.
//   rename "<name>"                — write name to the given string
//                                    (push EditObjectFieldCommand).
//                                    Empty-string arg AND name-unchanged
//                                    are both no-ops. The "" sentinel
//                                    here means "no rename happened";
//                                    scripts that genuinely want to
//                                    clear a name to the "" empty
//                                    sentinel (matching the newly-
//                                    minted state from `objects new`)
//                                    can't do so via `rename` today —
//                                    that's the same constraint
//                                    LayerProxy::rename has and matches
//                                    the LayersPanel's "empty entry
//                                    leaves the name alone" UX
//                                    convention. Worth a future
//                                    `clear_name` if scripts need it.
//
//   All five push EditObjectFieldCommand on the global undo stack on
//   success. Command bodies resolve iid → SceneNode every replay
//   (find_by_iid, project-wide); deleted-between-push-and-replay
//   degrades to a silent skip, same partial-recovery shape
//   EditLayerFieldCommand uses.
//
//   (m4 will add element-structural verbs: move, reparent, duplicate.
//   m5 closes with collection-level grouping: group, ungroup.)
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
// `new` creates into the active doc's active layer specifically;
// scripts that want to add to a different layer's children today must
// activate that layer first (`layers.<iid> activate`). That's the
// same constraint canvas tools observe.
//
// `delete` finds the iid wherever it lives across documents — the
// same project-wide find_by_iid resolution element queries use.
// Symmetric with `layers delete`, which also resolves by iid first
// and only then locates the owning doc.
//
// ── Why no rename and no find_by_iid query ──────────────────────────────
//
// Rename: that's an element mutating verb (m3). Reading via `get
// objects.<iid> name` covers the read side.
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
    // `history` is non-owning and may be nullptr. Captured in m1 for
    // shape-symmetry; ACTIVELY USED from m2 onwards — the `new` and
    // `delete` verbs push AddNodeCommand / DeleteObjectCommand on
    // every successful invocation, and m3's element mutating verbs
    // (toggle_visible / set_visible / toggle_locked / set_locked /
    // rename) push EditObjectFieldCommand. A null history degrades
    // every verb to direct mutation without undo support (the same
    // graceful-degradation pattern guides uses).
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
    Curvz::CommandHistory* m_history;   // non-owning; captured in m1
                                        // for shape-symmetry, actively
                                        // used from m2 onwards by `new`
                                        // and `delete` and from m3
                                        // onwards by the element mutating
                                        // verbs on the proxy. Null
                                        // history degrades every verb
                                        // to direct mutation.
};

} // namespace curvz::scripting
