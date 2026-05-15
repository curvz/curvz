// scripting/LayersScriptable.hpp ─────────────────────────────────────────────
//
// s216 m1 — first concrete row-bound model Scriptable. Wraps the
// project's layer surface as a collection Scriptable under abbrev
// `layers`, materialising transient `layers.<iid>` proxies for per-
// instance operations via the s216 m1 router hooks
// (Scriptable::can_resolve / proxy_for).
//
// This is the implementation pilot for the design banked in CANON's
// "Row-bound model Scriptables — collection-as-router with transient
// per-instance proxies" entry. The four remaining 5a collections
// (guides, swatches, styles, themes, objects) follow the same shape
// in subsequent sessions.
//
// ── Addressing ──────────────────────────────────────────────────────────────
//
//   layers                  — the collection Scriptable. Set verbs and
//                             set queries, plus iid enumeration and
//                             name lookup. ONE registry entry per app
//                             session; held as a member by MainWindow,
//                             registered for the lifetime of the window.
//
//   layers.<iid>             — per-instance proxy, materialised on
//                             demand by the collection. Reads / writes
//                             the specific layer's properties through
//                             the project's iid index. NOT registered
//                             — the registry only knows about `layers`.
//                             A `list` from the Scripter shows `layers`
//                             but never shows `layers.<iid>` entries
//                             (one of the pilot's success conditions).
//
// ── Lifetime and the project pointer ────────────────────────────────────────
//
// The LayersScriptable holds a project getter — `std::function<CurvzProject*()>`
// — rather than a raw project pointer. This is deliberate: MainWindow's
// `m_project` is a `unique_ptr<CurvzProject>` that gets reset
// (`on_close_project`) and reassigned (`load_project`) at runtime.
// Capturing a raw pointer at construction would dangle the first time
// the user opens a different project; the getter resolves to whatever's
// current at each verb call.
//
// MainWindow constructs the LayersScriptable once (after m_project is
// initialised in setup_project) and the registry holds the entry for
// the window's lifetime. Every verb invocation queries the getter
// fresh — the project's iid index is the truth, the Scriptable just
// dispatches.
//
// ── The proxy ───────────────────────────────────────────────────────────────
//
// LayerProxy is a private class inside LayersScriptable.cpp. Constructed
// by `proxy_for(iid)`, returned as a `unique_ptr<Scriptable>` to the
// listener's ResolvedObject wrapper, destroyed when the wrapper goes
// out of scope (end of `run_line`). It holds:
//
//   - the project getter (shared with the parent LayersScriptable so
//     mutations resolve through the current project)
//   - the iid (the per-instance key)
//
// Every proxy verb/query body calls `curvz::utils::find_by_iid(proj, iid)`
// fresh — no caching of the live SceneNode pointer. If the iid no
// longer resolves (layer deleted between dispatch lines), the proxy's
// invoke returns Null and query returns Null too, matching the
// "addressability tracks current live state" invariant.
//
// ── Verb / query surface ────────────────────────────────────────────────────
//
// COLLECTION VERBS (on `layers`):
//   new                                  — create a new layer, returns iid
//   delete "<iid>"                       — remove the layer
//   move "<iid>" "up"|"down"|"top"|"bottom"
//                                        — reorder
//   hide_others "<iid>"                  — visible=false on every other
//                                          real layer (Type::Layer only —
//                                          guide/grid/margin layers
//                                          ignored)
//   show_all                             — visible=true on every real
//                                          layer
//
// COLLECTION QUERIES (on `layers`):
//   count                                — number of Type::Layer entries
//                                          in the active document
//   all_iids                             — comma-separated iids (top-down
//                                          order). ScriptValue has no
//                                          array type yet; the canon
//                                          expects a future `foreach`
//                                          listener-grammar extension
//                                          to consume this. For the
//                                          pilot the string is a sentinel
//                                          (presence indicates the
//                                          query works; scripts can't
//                                          iterate it without grammar
//                                          changes).
//   active_iid                           — iid of the active layer, or
//                                          "" if no active layer or no
//                                          active document
//   find_by_name "<name>"                — iid of the first Type::Layer
//                                          with this exact name, or ""
//                                          on miss. Names aren't unique
//                                          by construction (the doc-
//                                          name-uniquifier helps in
//                                          practice but isn't enforced),
//                                          so "first hit" is the
//                                          contract — most existing
//                                          panels behave the same way.
//
// ELEMENT VERBS (on `layers.<iid>`):
//   toggle_visible                       — flip visible (push EditLayerFieldCommand)
//   set_visible <bool>                   — write visible (push EditLayerFieldCommand)
//   set_locked  <bool>                   — write locked  (push EditLayerFieldCommand)
//   toggle_locked                        — flip locked   (push EditLayerFieldCommand)
//   rename "<name>"                      — write name    (push EditLayerFieldCommand)
//   color "<hex>"                        — write color   (push EditLayerFieldCommand).
//                                          s217 m2 renamed from `set_color`
//                                          to match the inspector field
//                                          label. The old name still works
//                                          as a deprecated alias.
//   opacity <0..1>                       — write opacity directly. NO
//                                          undo today: EditLayerFieldCommand's
//                                          Field enum doesn't cover opacity.
//                                          Follow-up: extend the enum to
//                                          include Opacity (double-valued
//                                          field, parallel to existing
//                                          Visible/Locked bool fields).
//                                          The verb works correctly on
//                                          the model; only undo/redo is
//                                          missing. s217 m2 renamed from
//                                          `set_opacity` to match the
//                                          inspector slider label. The old
//                                          name still works as a deprecated
//                                          alias.
//   activate                             — set this layer as active.
//                                          Mutates active_layer_index
//                                          directly; no command pushed
//                                          (the panel doesn't push for
//                                          active-layer changes either).
//                                          s217 m2 renamed from
//                                          `make_active` (the user verb
//                                          for clicking a layer row, not
//                                          the API verb). The old name
//                                          still works as a deprecated
//                                          alias.
//
// ELEMENT QUERIES (on `layers.<iid>`):
//   name           — std::string layer.name
//   visible        — bool         layer.visible
//   locked         — bool         layer.locked
//   opacity        — double       layer.opacity
//   color          — std::string  layer.color (hex like "#e34c26", or "")
//   child_count    — int          layer.children.size()
//   iid            — std::string  layer.internal_id (for completeness:
//                                 a script can `get layers.<iid> iid` to
//                                 verify it has the right handle, useful
//                                 in tests)
//
// ── On the active-document scope ────────────────────────────────────────────
//
// Collection verbs operate on `project->active_doc()`. Element verbs
// resolve through `curvz::utils::find_by_iid(project, iid)`, which
// walks every document in the project. So:
//
//   - `layers count`             — counts the ACTIVE doc's layers.
//   - `layers new`               — adds to the ACTIVE doc.
//   - `layers.<iid> set_visible`  — finds the iid in WHICHEVER doc holds
//                                  it (could be a non-active doc — the
//                                  iid is the address, the doc is just
//                                  where the iid lives).
//
// This matches CANON's "Cross-document scope" section: `layers.<iid>` is
// global across the project, but verbs that need a document scope
// (`new`, `count`) read from the project's active state. A future
// `proj` singleton will provide the verb for switching active docs.
//
// ── Diagnostic-only ─────────────────────────────────────────────────────────
//
// This header is included only from `.cpp` files compiled in the
// CURVZ_DIAGNOSTIC build; production builds don't see it. MainWindow's
// LayersScriptable member is held behind `#ifdef CURVZ_DIAGNOSTIC` so
// the production MainWindow header stays clean.

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

class LayersScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;

    // Registers as "layers" via the Scriptable base ctor. The project
    // getter is called fresh on every verb invocation — never cached —
    // so MainWindow can swap its m_project unique_ptr freely (open
    // another project, close project) without invalidating us. The
    // getter is allowed to return nullptr; verbs degrade gracefully
    // (count returns 0, find_by_name returns "", etc.).
    //
    // `history` is non-owning and may be nullptr (test harness path).
    // Lives at MainWindow scope alongside the project unique_ptr; the
    // CommandHistory object itself has a stable address (it's a value
    // member, not a pointer that gets swapped), so storing a raw
    // pointer is safe for the LayersScriptable's lifetime.
    LayersScriptable(ProjectGetter get_project,
                     Curvz::CommandHistory* history);
    ~LayersScriptable() override = default;

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
    Curvz::CommandHistory* m_history;   // non-owning; outlives us
};

} // namespace curvz::scripting
