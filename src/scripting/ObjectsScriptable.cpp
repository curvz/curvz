// scripting/ObjectsScriptable.cpp ────────────────────────────────────────────
//
// s230 m1 — implementation of the sixth row-bound model Scriptable.
// See ObjectsScriptable.hpp for the verb/query surface, lifetime
// notes, the scope choice (real scene contents only — option (ii)
// from the s230 handoff's design fork), and the "READ-SIDE only"
// scope of m1.
//
// s231 m2 — adds collection-level structural verbs `new <type>` and
// `delete <iid>` so the smoke can mint and tear down its own test
// objects. The element queries from m1 now get exercised via a full
// proxy round-trip on iids the smoke itself created.
//
// s232 m3 — wires five element mutating verbs onto `ObjectProxy::invoke`,
// which was a no-op stub through m1+m2. Each verb (toggle_visible,
// set_visible, toggle_locked, set_locked, rename) does the direct
// mutation on the SceneNode field then pushes an EditObjectFieldCommand
// (a new command class added in m3, direct analog of
// EditLayerFieldCommand minus the Color field). Mirrors
// LayerProxy::invoke's shape exactly — see LayersScriptable.cpp for
// the parallel implementation. Helpers `push_bool_field` /
// `push_string_field` on the proxy match the layer-side helpers
// of the same names; `arg_as_bool` is added to the anon namespace.
//
// ── Command pushes (s231 m2) ─────────────────────────────────────────────
//
// `new` pushes an AddNodeCommand; `delete` pushes a DeleteObjectCommand.
// Both commands existed before m2 (canvas tools have been using them
// since well before the iid migration); m2 wires them into a new code
// path without changing their shape. Each command captures the parent
// as a raw SceneNode* — same legacy capture LayersScriptable's `new`
// and Canvas's tool-driven AddNodeCommand use. The s167 migration
// was specifically about EditPathCommand and friends; structural
// commands (Add / Delete / Insert) retain their pre-migration capture
// shape and remain in scope for a future structural-iid migration.
//
// Direct mutation is performed BEFORE the command push, mirroring
// LayersScriptable::new — the user's perspective is "the verb did
// the thing"; Ctrl+Z then UNDOES it. The push site does not call
// command->execute() (that's the redo path, only invoked via the
// history's redo()).
//
// After every direct mutation, `doc->invalidate_iid_index()` runs so
// the lazy iid map rebuilds on the next find_by_iid call. The s168 m6
// "invalidate before resolve" inside each command's execute/undo path
// covers redo / undo themselves; the push site's invalidation covers
// the initial mutation.
//
// ── find_by_name and find_by_type are invoke-shaped ──────────────────────
//
// Same grammar constraint LayersScriptable surfaces: today's query()
// can't take args. `get objects find_by_name "X"` would put
// "find_by_name" in the property slot and "X" would be ignored. We
// expose find_by_name and find_by_type as INVOKE verbs on the
// collection — read-shaped semantics, invoke-shaped dispatch.
// Scripts write `objects find_by_name "X"` (no `get` prefix) and
// pick up the result through the `=` output line / `set X to result`
// binding. Future grammar extension: when query() grows an args
// parameter, both can graduate to real queries.
//
// ── Tree walk ────────────────────────────────────────────────────────────
//
// Several queries (count, all_iids, find_by_name, find_by_type) walk
// every layer's subtree depth-first. The walk is file-local because
// it's a small helper and the iteration shape (filter by
// is_scene_object) is specific to this Scriptable's scope choice.
// If a future Scriptable needs the same walk it can be promoted to
// curvz_utils — but right now this is the only caller, and the
// helper stays adjacent to the predicate it uses.
//
// The walk visits every node in the tree rooted at each top-level
// layer, applies a visitor function, and recurses into `children`
// and the structural-input slots (clip_shape, blend_source_a / _b,
// warp_source). The non-children slots matter because a ClipGroup's
// clip_shape, a Blend's source nodes, and a Warp's source node are
// all "real" scene nodes with iids that should be addressable. The
// derived caches (blend_cache, warp_cache, warp_glyph_cache) are
// NOT walked — same exclusion as CurvzDocument's m_iid_index,
// because iids inside caches aren't stable across rebuilds.
//
// ── parent_iid walk ──────────────────────────────────────────────────────
//
// The element query `parent_iid` needs the immediate parent container
// of a given node. Canvas.cpp has a `find_parent` helper but it only
// descends one level into Group/Compound — insufficient for the full
// tree shape we cover (ClipGroup, Blend, Warp containers; nested
// Group-inside-Group). The file-local parent walk here is a clean
// recursive scan: visit every container slot, check whether the
// target lives directly under it, return the container's iid if so.
//
// O(N) per call where N is the tree size. The number of parent_iid
// queries in a script is bounded by however many `get objects.<iid>
// parent_iid` lines the author writes; the walk cost is microseconds
// on typical scenes (hundreds of nodes). If parent_iid becomes hot
// or a sibling Scriptable wants the same walk, promote to
// curvz_utils as `find_parent_iid` — same iid-resolution shape as
// find_by_iid, but returning the owner's iid instead of the node.
//
// s231 m2 adds a SIBLING walker `find_parent_node_in_*` that returns
// the parent SceneNode pointer instead of its iid (needed by `delete`
// to manipulate the parent's children vector and capture the
// original index for DeleteObjectCommand). The two walkers share the
// same recursive structure; both file-local. If a future caller
// wants both pieces of information out of a single walk, the seam
// for a promoted helper is clear — but two single-purpose walkers
// is cheaper today than one general one.

#include "scripting/ObjectsScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "SceneNode.hpp"
#include "curvz_utils.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

// ── Scope predicate and type-string mapping ──────────────────────────────
//
// File-local helpers. is_scene_object decides whether a SceneNode
// belongs in the `objects` collection (option (ii) from the s230
// design fork — real scene contents only, no Layer/Guide/special-
// layer types). type_to_string returns the lowercase token used in
// find_by_type args and in the `type` element query.

namespace {

bool is_scene_object(const Curvz::SceneNode* n) {
    if (!n) return false;
    using T = Curvz::SceneNode::Type;
    switch (n->type) {
        case T::Path:
        case T::Group:
        case T::Compound:
        case T::ClipGroup:
        case T::Blend:
        case T::Warp:
        case T::Text:
        case T::Image:
        case T::Ref:
        case T::Measurement:
            return true;
        case T::Layer:
        case T::Guide:
        case T::GuideLayer:
        case T::RefLayer:
        case T::MeasureLayer:
        case T::GridLayer:
        case T::MarginLayer:
            return false;
    }
    return false;
}

// Lowercase type-token vocabulary. Stable across the Scriptable's
// lifetime (part of the documented contract — see header). The
// inverse mapping in find_by_type uses the same strings.
std::string type_to_string(Curvz::SceneNode::Type t) {
    using T = Curvz::SceneNode::Type;
    switch (t) {
        case T::Path:        return "path";
        case T::Group:       return "group";
        case T::Compound:    return "compound";
        case T::ClipGroup:   return "clipgroup";
        case T::Blend:       return "blend";
        case T::Warp:        return "warp";
        case T::Text:        return "text";
        case T::Image:       return "image";
        case T::Ref:         return "ref";
        case T::Measurement: return "measurement";
        // Out-of-scope types — type_to_string never gets called on
        // these in m1 because callers gate on is_scene_object first.
        // Return "" anyway so an unanticipated caller path doesn't
        // fall through to UB; "" is the "not in scope" sentinel
        // matching how find_by_name / find_by_type signal miss.
        case T::Layer:
        case T::Guide:
        case T::GuideLayer:
        case T::RefLayer:
        case T::MeasureLayer:
        case T::GridLayer:
        case T::MarginLayer:
            return "";
    }
    return "";
}

// Pull a string-shaped arg (quoted or token). Returns "" for any
// non-string ScriptValue. Matches the equivalent helper in
// LayersScriptable.cpp and GuidesScriptable.cpp.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

// Parse a bool from a ScriptValue. Accepts Bool directly; falls back
// to text "true"/"false" (case-sensitive — matches the DSL literal
// lexer). Returns `fallback` for anything else. Mirrors the
// LayersScriptable.cpp helper of the same name (s232 m3 — added
// when element mutating verbs landed on the proxy).
bool arg_as_bool(const ScriptValue& v, bool fallback) {
    if (v.kind == ValueKind::Bool)   return v.b;
    if (v.kind == ValueKind::String) {
        if (v.s == "true")  return true;
        if (v.s == "false") return false;
    }
    return fallback;
}

// ── Scene-tree walker ────────────────────────────────────────────────────
//
// Depth-first visit of every node under each top-level layer in `doc`,
// calling `visit(SceneNode*)` for each. Visits structural-input slots
// (clip_shape, blend sources, warp source) in addition to children,
// matching the iid-index walk coverage documented in CurvzDocument's
// m_iid_index comment.
//
// The visitor is called for EVERY node (in-scope and out-of-scope);
// callers gate on is_scene_object themselves. Keeps the walker
// reusable for future Scriptables that want a different filter.
//
// Early-exit support via a bool return — visitors that return true
// signal "found what I want, stop walking." Useful for find_by_*
// implementations.
template <class F>
bool walk_doc_tree(Curvz::CurvzDocument* doc, F&& visit) {
    if (!doc) return false;
    std::function<bool(Curvz::SceneNode*)> rec =
        [&](Curvz::SceneNode* n) -> bool {
            if (!n) return false;
            if (visit(n)) return true;
            for (auto& c : n->children) if (rec(c.get())) return true;
            if (n->clip_shape     && rec(n->clip_shape.get()))     return true;
            if (n->blend_source_a && rec(n->blend_source_a.get())) return true;
            if (n->blend_source_b && rec(n->blend_source_b.get())) return true;
            if (n->warp_source    && rec(n->warp_source.get()))    return true;
            return false;
        };
    for (auto& l : doc->layers) {
        if (rec(l.get())) return true;
    }
    return false;
}

// ── Parent walk ──────────────────────────────────────────────────────────
//
// Find the immediate parent of `target` in the doc tree. The parent
// is whichever container slot owns `target` — `children[i]`,
// `clip_shape`, `blend_source_a`, `blend_source_b`, or `warp_source`.
// Returns the parent's iid, or "" if no container holds target.
//
// "Container slot owns target" means: target is the .get() of some
// unique_ptr held by the parent. We compare pointers, not iids —
// pointer identity is the canonical "is this the slot that owns
// you" test for unique_ptr-tree shapes.
std::string find_parent_iid_in_doc(Curvz::CurvzDocument* doc,
                                   Curvz::SceneNode* target) {
    if (!doc || !target) return {};
    std::string result;
    std::function<bool(Curvz::SceneNode*)> rec =
        [&](Curvz::SceneNode* n) -> bool {
            if (!n) return false;
            for (auto& c : n->children) {
                if (c.get() == target) {
                    result = n->internal_id;
                    return true;
                }
            }
            if (n->clip_shape && n->clip_shape.get() == target) {
                result = n->internal_id;
                return true;
            }
            if (n->blend_source_a && n->blend_source_a.get() == target) {
                result = n->internal_id;
                return true;
            }
            if (n->blend_source_b && n->blend_source_b.get() == target) {
                result = n->internal_id;
                return true;
            }
            if (n->warp_source && n->warp_source.get() == target) {
                result = n->internal_id;
                return true;
            }
            // Recurse: descend into every owned slot.
            for (auto& c : n->children)
                if (rec(c.get())) return true;
            if (n->clip_shape && rec(n->clip_shape.get())) return true;
            if (n->blend_source_a && rec(n->blend_source_a.get())) return true;
            if (n->blend_source_b && rec(n->blend_source_b.get())) return true;
            if (n->warp_source && rec(n->warp_source.get())) return true;
            return false;
        };
    for (auto& l : doc->layers) {
        if (rec(l.get())) return result;
    }
    return {};
}

// Project-wide parent search: the iid we hold may live in any doc.
// Walk each doc until one finds the target node and returns its
// parent iid.
std::string find_parent_iid_in_project(Curvz::CurvzProject* proj,
                                       Curvz::SceneNode* target) {
    if (!proj || !target) return {};
    for (auto& doc_up : proj->documents) {
        auto* doc = doc_up.get();
        std::string p = find_parent_iid_in_doc(doc, target);
        if (!p.empty()) return p;
    }
    return {};
}

// ── Parent-node walk (s231 m2 — sibling of the iid walker) ───────────────
//
// Same recursive shape as find_parent_iid_in_doc, but returns the
// parent SceneNode* AND the index of `target` within the parent's
// children vector. `delete` needs both: the SceneNode* to construct
// the DeleteObjectCommand (which captures `parent` directly), and
// the index for the command's undo path so the snapshot re-inserts
// at the original position.
//
// If the target is owned by a non-`children` slot (clip_shape,
// blend_source_a, blend_source_b, warp_source), `index_out` is set
// to -1 and the parent pointer still returns. delete refuses to
// proceed in that case — pulling a node out of a structural slot
// would dissolve the containing ClipGroup / Blend / Warp, which is
// a different surface than "delete a scene leaf." Scheduled for a
// later milestone if scripts genuinely need to dissolve clip /
// blend / warp containers via `objects delete`.
struct ParentSlot {
    Curvz::SceneNode* parent;
    int               child_index;  // -1 if target lives in a non-children slot
};

ParentSlot find_parent_node_in_doc(Curvz::CurvzDocument* doc,
                                   Curvz::SceneNode* target) {
    ParentSlot found{nullptr, -1};
    if (!doc || !target) return found;
    std::function<bool(Curvz::SceneNode*)> rec =
        [&](Curvz::SceneNode* n) -> bool {
            if (!n) return false;
            for (int i = 0; i < (int)n->children.size(); ++i) {
                if (n->children[i].get() == target) {
                    found = {n, i};
                    return true;
                }
            }
            if (n->clip_shape && n->clip_shape.get() == target) {
                found = {n, -1};
                return true;
            }
            if (n->blend_source_a && n->blend_source_a.get() == target) {
                found = {n, -1};
                return true;
            }
            if (n->blend_source_b && n->blend_source_b.get() == target) {
                found = {n, -1};
                return true;
            }
            if (n->warp_source && n->warp_source.get() == target) {
                found = {n, -1};
                return true;
            }
            for (auto& c : n->children)
                if (rec(c.get())) return true;
            if (n->clip_shape && rec(n->clip_shape.get())) return true;
            if (n->blend_source_a && rec(n->blend_source_a.get())) return true;
            if (n->blend_source_b && rec(n->blend_source_b.get())) return true;
            if (n->warp_source && rec(n->warp_source.get())) return true;
            return false;
        };
    for (auto& l : doc->layers) {
        if (rec(l.get())) return found;
    }
    return found;
}

ParentSlot find_parent_node_in_project(Curvz::CurvzProject* proj,
                                       Curvz::SceneNode* target) {
    if (!proj || !target) return {nullptr, -1};
    for (auto& doc_up : proj->documents) {
        auto* doc = doc_up.get();
        ParentSlot p = find_parent_node_in_doc(doc, target);
        if (p.parent) return p;
    }
    return {nullptr, -1};
}

// ── Type-token → SceneNode::Type (s231 m2) ───────────────────────────────
//
// Inverse of type_to_string for the small subset of types `new` can
// mint in m2. Returns nullopt for unrecognised tokens or for types
// that aren't safely creatable from scratch via this verb today
// (Compound / ClipGroup / Blend / Warp need structural inputs;
// Text / Image need content; Ref / Measurement live under special
// layers). Those are reachable later milestones; m2 ships path +
// group, the two leaf-vs-container types that are minimal to mint.
//
// std::optional avoided to keep the header surface clean — sentinel
// is the bool return; the out-param carries the type.
bool parse_type_token(std::string_view tok, Curvz::SceneNode::Type& out) {
    using T = Curvz::SceneNode::Type;
    if (tok == "path")  { out = T::Path;  return true; }
    if (tok == "group") { out = T::Group; return true; }
    return false;
}

// ── Mint a fresh SceneNode of the given type (s231 m2) ───────────────────
//
// File-local factory. Sets the bare-minimum fields each type needs to
// be a valid in-scope scene object — type set, internal_id minted
// (default ctor already does this; we don't re-mint), name LEFT EMPTY
// (the documented contract — scripts use rename to assign a name),
// children empty by default ctor.
//
// Path-specific: allocates a default-constructed PathData so the
// node is structurally a Path (path != nullptr is the discriminant
// renderers and serialisers rely on). The PathData has no nodes; the
// renderer skips empty paths defensively (Canvas_draw.cpp / SvgWriter
// both check nodes.empty()).
//
// Group-specific: no extra setup beyond type — children vector is
// already empty.
std::unique_ptr<Curvz::SceneNode>
mint_scene_object(Curvz::SceneNode::Type t) {
    auto n = std::make_unique<Curvz::SceneNode>();
    // internal_id and a default name vocabulary are NOT touched —
    // default ctor mints internal_id, and the contract is name == ""
    // until a future rename verb assigns one.
    n->type = t;
    n->name.clear();  // explicit: documented "newly created" sentinel
    if (t == Curvz::SceneNode::Type::Path) {
        n->path = std::make_unique<Curvz::PathData>();
    }
    return n;
}

} // anon namespace

// ── ObjectProxy — transient per-instance Scriptable ──────────────────────
//
// Materialised on demand by ObjectsScriptable::proxy_for(iid).
// Lifetime is bounded by the listener's ResolvedObject wrapper —
// destroyed when the wrapper goes out of scope at end-of-statement.
//
// Registered via the `unregistered` tag — proxies are invisible to
// the global registry. Same lifetime contract and registry posture
// as LayerProxy / GuideProxy.
//
// m1 + m2 shipped READ-SIDE only. m3 wires invoke() to the five
// element mutating verbs: toggle_visible / set_visible / toggle_locked
// / set_locked / rename. Each pushes EditObjectFieldCommand on the
// global undo stack on success (direct mutation in the verb body
// happens BEFORE the push, mirroring LayerProxy::invoke).
class ObjectProxy : public Scriptable {
public:
    ObjectProxy(ObjectsScriptable::ProjectGetter get_project,
                Curvz::CommandHistory* history,
                std::string iid)
        : Scriptable(unregistered)
        , m_get_project(std::move(get_project))
        , m_history(history)
        , m_iid(std::move(iid)) {}

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

private:
    // Resolve our iid to a live SceneNode through the current project,
    // filtered to is_scene_object. Returns nullptr if the iid no
    // longer addresses a scene-object-typed node (deleted, somehow
    // re-typed to a Layer / Guide / special-layer type).
    Curvz::SceneNode* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        auto* n = curvz::utils::find_by_iid(*proj, m_iid);
        return is_scene_object(n) ? n : nullptr;
    }

    // Push an EditObjectFieldCommand with our iid. The DIRECT mutation
    // must happen at the caller site (mirrors the LayersPanel /
    // LayerProxy pattern: mutate the SceneNode field, then push the
    // command with before/after). Skips silently if history isn't
    // wired or proj is missing — same fallback shape as LayerProxy.
    // (s232 m3 — direct mirror of LayerProxy's helpers of the same
    // names. EditObjectFieldCommand is the analog of
    // EditLayerFieldCommand minus the Color field.)
    void push_bool_field(Curvz::EditObjectFieldCommand::Field field,
                         bool before, bool after) {
        if (!m_history) return;
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return;
        if (m_iid.empty()) return;
        auto cmd = std::make_unique<Curvz::EditObjectFieldCommand>(
            proj, m_iid, field,
            /*before_str=*/std::string{}, /*after_str=*/std::string{},
            before, after);
        m_history->push(std::move(cmd));
    }
    void push_string_field(Curvz::EditObjectFieldCommand::Field field,
                           const std::string& before,
                           const std::string& after) {
        if (!m_history) return;
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return;
        if (m_iid.empty()) return;
        auto cmd = std::make_unique<Curvz::EditObjectFieldCommand>(
            proj, m_iid, field, before, after,
            /*before_bool=*/false, /*after_bool=*/false);
        m_history->push(std::move(cmd));
    }

    ObjectsScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*           m_history;   // wired in m3 — pushes
                                                  // EditObjectFieldCommand
                                                  // for the five element
                                                  // mutating verbs
    std::string                      m_iid;
};

// ── ObjectProxy: invoke ──────────────────────────────────────────────────
//
// s232 m3 — five element mutating verbs. Direct mirror of
// LayerProxy::invoke's shape, swapping EditLayerFieldCommand for
// EditObjectFieldCommand. Direct mutation precedes the command push;
// no-op (no command) when the new value matches the old. `rename`
// also no-ops on empty-string args — matches LayerProxy::rename's
// "empty entry leaves the name alone" UX convention.

ScriptValue ObjectProxy::invoke(std::string_view verb,
                                const ScriptArgs& args) {
    using Field = Curvz::EditObjectFieldCommand::Field;
    auto* node = resolve();
    if (!node) return ScriptValue::null();

    if (verb == "toggle_visible") {
        bool before = node->visible;
        bool after  = !before;
        node->visible = after;
        push_bool_field(Field::Visible, before, after);
        return ScriptValue::null();
    }

    if (verb == "set_visible") {
        if (args.empty()) return ScriptValue::null();
        bool before = node->visible;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();  // no-op, no command
        node->visible = after;
        push_bool_field(Field::Visible, before, after);
        return ScriptValue::null();
    }

    if (verb == "toggle_locked") {
        bool before = node->locked;
        bool after  = !before;
        node->locked = after;
        push_bool_field(Field::Locked, before, after);
        return ScriptValue::null();
    }

    if (verb == "set_locked") {
        if (args.empty()) return ScriptValue::null();
        bool before = node->locked;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();
        node->locked = after;
        push_bool_field(Field::Locked, before, after);
        return ScriptValue::null();
    }

    if (verb == "rename") {
        if (args.empty()) return ScriptValue::null();
        std::string before = node->name;
        std::string after  = arg_as_string(args[0]);
        if (after.empty() || before == after) return ScriptValue::null();
        node->name = after;
        push_string_field(Field::Name, before, after);
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── ObjectProxy: query ───────────────────────────────────────────────────

ScriptValue ObjectProxy::query(std::string_view property) const {
    auto* node = resolve();
    if (!node) return ScriptValue::null();

    if (property == "name")        return ScriptValue::text(node->name);
    if (property == "type")        return ScriptValue::text(type_to_string(node->type));
    if (property == "visible")     return ScriptValue::boolean(node->visible);
    if (property == "locked")      return ScriptValue::boolean(node->locked);
    if (property == "child_count") return ScriptValue::integer(
                                       static_cast<long long>(node->children.size()));
    if (property == "iid")         return ScriptValue::text(node->internal_id);

    if (property == "parent_iid") {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::text("");
        return ScriptValue::text(find_parent_iid_in_project(proj, node));
    }

    return ScriptValue::null();
}

std::vector<std::string> ObjectProxy::verbs() const {
    return {
        // s232 m3 — five element mutating verbs. Each pushes
        // EditObjectFieldCommand on success; direct mutation
        // happens in the verb body before the push.
        "toggle_visible",
        "set_visible",
        "toggle_locked",
        "set_locked",
        "rename",
        // m4 will add element-structural verbs (move, reparent,
        // duplicate). m5 closes with collection-level grouping
        // (group, ungroup) — those live on the collection, not
        // the proxy.
    };
}

std::vector<std::string> ObjectProxy::properties() const {
    return {
        "name", "type", "visible", "locked",
        "parent_iid", "child_count", "iid",
    };
}

// ── ObjectsScriptable ────────────────────────────────────────────────────

ObjectsScriptable::ObjectsScriptable(ProjectGetter get_project,
                                     Curvz::CommandHistory* history)
    : Scriptable("objects")
    , m_get_project(std::move(get_project))
    , m_history(history) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "objects". MainWindow holds us as a member; the
    // registry entry lives for the window's lifetime.
}

// ── Router hooks ─────────────────────────────────────────────────────────

bool ObjectsScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    auto* node = curvz::utils::find_by_iid(*proj, std::string(key));
    return is_scene_object(node);
}

std::unique_ptr<Scriptable>
ObjectsScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<ObjectProxy>(m_get_project, m_history,
                                         std::string(key));
}

// ── Collection invoke ────────────────────────────────────────────────────
//
// m1 had two read-shaped invoke verbs (find_by_name, find_by_type)
// living here because today's query() can't take args. m2 adds two
// MUTATING structural verbs (new, delete) — the first writes on the
// `objects` surface. See header note "find_by_name and find_by_type
// are invoke-shaped" above.

ScriptValue ObjectsScriptable::invoke(std::string_view verb,
                                      const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto* doc = proj->active_doc();
    if (!doc) return ScriptValue::null();

    if (verb == "find_by_name") {
        // Read-shaped verb — returns iid of the first in-scope scene
        // object whose `name` matches exactly, or "" on miss. Walks
        // the doc tree depth-first; first hit wins. Names aren't
        // unique by construction; matches LayersScriptable's
        // find_by_name "first hit" contract.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        if (target.empty()) return ScriptValue::text("");
        std::string found;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (!is_scene_object(node)) return false;
            if (node->name == target) {
                found = node->internal_id;
                return true;  // early exit
            }
            return false;
        });
        return ScriptValue::text(found);
    }

    if (verb == "find_by_type") {
        // Read-shaped verb — returns iid of the first in-scope scene
        // object whose type matches the supplied lowercase token, or
        // "" on miss / unknown type. The token vocabulary is exactly
        // the one type_to_string emits — "path" / "group" /
        // "compound" / "clipgroup" / "blend" / "warp" / "text" /
        // "image" / "ref" / "measurement". Out-of-scope types
        // (layer, guide, etc.) silently don't match — passing
        // "layer" returns "" rather than dispatching against the
        // out-of-scope set.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        if (target.empty()) return ScriptValue::text("");
        std::string found;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (!is_scene_object(node)) return false;
            if (type_to_string(node->type) == target) {
                found = node->internal_id;
                return true;  // early exit
            }
            return false;
        });
        return ScriptValue::text(found);
    }

    // ── new <type> (s231 m2) ─────────────────────────────────────────────
    //
    // Mints a fresh in-scope scene object of the given type and
    // inserts it at the FRONT of the active layer's children
    // (canvas convention — new-on-top). Returns the new iid as a
    // string so a script can `set <var> to result` and address the
    // node via `objects.<var>` from then on.
    //
    // Pushes an AddNodeCommand so Ctrl+Z removes the new node.
    // The command captures the parent (active layer) as a raw
    // pointer — same pre-iid-migration shape Canvas tools use
    // when pushing AddNodeCommand. A future structural-iid
    // migration will sweep these to CurvzProject* + iid captures
    // alongside Canvas's own push sites; until then the parent
    // pointer is durable for the lifetime of the active layer
    // (which doesn't get destroyed between push and undo under
    // normal use).
    //
    // No-op returns (text("")):
    //   - args.empty() — caller didn't supply a type token.
    //   - args[0] is not a string-shaped ScriptValue.
    //   - the type token isn't in the m2 vocabulary ("path" or
    //     "group"). Future milestones extend the vocabulary; today
    //     unknown tokens return "" rather than erroring, matching
    //     the find_by_type miss contract.
    //   - active_doc has no active layer (defensive — should not
    //     happen with a real project but degrade gracefully).
    if (verb == "new") {
        if (args.empty()) return ScriptValue::text("");
        std::string type_tok = arg_as_string(args[0]);
        if (type_tok.empty()) return ScriptValue::text("");
        Curvz::SceneNode::Type new_type{};
        if (!parse_type_token(type_tok, new_type)) {
            return ScriptValue::text("");
        }
        auto* active = doc->active_layer();
        if (!active) return ScriptValue::text("");

        auto fresh = mint_scene_object(new_type);
        std::string new_iid = fresh->internal_id;

        // Snapshot for the command BEFORE inserting — clone_node
        // preserves internal_id so the command's redo (a future
        // Ctrl+Y) re-inserts a node with the same iid. The original
        // unique_ptr moves into the children vector; the cloned
        // unique_ptr is what the command owns.
        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history) {
            snap_for_cmd = Curvz::clone_node(*fresh);
        }

        active->children.insert(active->children.begin(),
                                std::move(fresh));
        doc->invalidate_iid_index();

        if (snap_for_cmd) {
            auto cmd = std::make_unique<Curvz::AddNodeCommand>(
                active, std::move(snap_for_cmd));
            m_history->push(std::move(cmd));
        }
        return ScriptValue::text(new_iid);
    }

    // ── delete <iid> (s231 m2) ───────────────────────────────────────────
    //
    // Removes the in-scope scene object identified by iid from its
    // owning container. Resolves the iid project-wide (the node may
    // live in any open doc, same shape as `layers delete`). Refuses
    // to delete nodes owned by a structural slot (clip_shape,
    // blend_source_a, blend_source_b, warp_source) — pulling a node
    // out of those slots would dissolve the containing ClipGroup /
    // Blend / Warp, which is a different surface than the leaf-
    // delete this verb implements. Scheduled for a later milestone.
    //
    // Pushes a DeleteObjectCommand so Ctrl+Z restores the node at
    // its original child-index. Scrubs the global undo stack for
    // raw-pointer-capture references to the about-to-be-deleted
    // node and its descendants — same defensive walk
    // LayersScriptable::delete uses.
    //
    // No-op returns (null):
    //   - args.empty() — caller didn't supply an iid.
    //   - args[0] is not a string-shaped ScriptValue.
    //   - iid doesn't resolve to an in-scope scene object (already
    //     deleted, never existed, or names a layer/guide/special-
    //     layer iid that `objects` doesn't own).
    //   - the resolved node has no findable parent in any doc
    //     (shouldn't happen for a resolved iid — defensive).
    //   - the node is owned by a non-children slot (see above).
    if (verb == "delete") {
        if (args.empty()) return ScriptValue::null();
        std::string target_iid = arg_as_string(args[0]);
        if (target_iid.empty()) return ScriptValue::null();
        auto* node = curvz::utils::find_by_iid(*proj, target_iid);
        if (!is_scene_object(node)) return ScriptValue::null();

        ParentSlot slot = find_parent_node_in_project(proj, node);
        if (!slot.parent) return ScriptValue::null();
        // Refuse non-children-owned targets — see header note above.
        if (slot.child_index < 0) return ScriptValue::null();

        // Defensive scrub: any LEGACY raw-pointer-capture command on
        // the undo stack that references this node (or any of its
        // descendants) gets neutralised before we erase. Same shape
        // LayersScriptable::delete uses — defensive and cheap. The
        // s167 iid migration covers EditPathCommand and friends, but
        // older commands (and any non-migrated ones) may still hold
        // raw pointers; scrub_command_history walks the stack and
        // neutralises those captures by predicate.
        if (m_history) {
            Curvz::SceneNode* to_delete = node;
            std::function<void(Curvz::SceneNode*)> scrub_walk =
                [&](Curvz::SceneNode* n) {
                    if (!n) return;
                    m_history->scrub_command_history(n);
                    for (auto& c : n->children) scrub_walk(c.get());
                    if (n->clip_shape)     scrub_walk(n->clip_shape.get());
                    if (n->blend_source_a) scrub_walk(n->blend_source_a.get());
                    if (n->blend_source_b) scrub_walk(n->blend_source_b.get());
                    if (n->warp_source)    scrub_walk(n->warp_source.get());
                };
            scrub_walk(to_delete);
        }

        // Snapshot for the command BEFORE erasing — clone_node
        // preserves internal_id so undo re-inserts a node with the
        // same iid (the script's bound variable still resolves
        // post-undo).
        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history) {
            snap_for_cmd = Curvz::clone_node(*node);
        }

        // Erase by iid match on the parent's children vector — the
        // index we captured is also the natural target, but iid
        // match is the safer comparison if anything inserted between
        // the find and the erase (vanishingly unlikely in this
        // single-threaded path; defensive).
        auto& ch = slot.parent->children;
        int erase_idx = slot.child_index;
        for (int i = 0; i < (int)ch.size(); ++i) {
            if (ch[i].get() == node) { erase_idx = i; break; }
        }
        // Find which doc owns the parent so we can invalidate its
        // iid index. The parent walk already told us about the
        // doc-by-finding-it; find_doc_by_iid on the parent's iid is
        // the cheapest reuse of existing curvz_utils.
        auto* owning_doc = curvz::utils::find_doc_by_iid(
            *proj, slot.parent->internal_id);
        ch.erase(ch.begin() + erase_idx);
        if (owning_doc) owning_doc->invalidate_iid_index();

        if (snap_for_cmd && m_history) {
            auto cmd = std::make_unique<Curvz::DeleteObjectCommand>(
                slot.parent, std::move(snap_for_cmd), erase_idx);
            m_history->push(std::move(cmd));
        }
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── Collection query ─────────────────────────────────────────────────────

ScriptValue ObjectsScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — defensible empties. Matches LayersScriptable /
        // GuidesScriptable: a test running before project open sees
        // 0 / "" not null.
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_iids") return ScriptValue::text("");
        return ScriptValue::null();
    }
    auto* doc = proj->active_doc();
    if (!doc) {
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_iids") return ScriptValue::text("");
        return ScriptValue::null();
    }

    if (property == "count") {
        long long n = 0;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (is_scene_object(node)) ++n;
            return false;  // no early exit — full walk
        });
        return ScriptValue::integer(n);
    }

    if (property == "all_iids") {
        // Tree-walk order (depth-first, layer-top-down). Same sentinel
        // shape as LayersScriptable / GuidesScriptable all_iids —
        // comma-separated string awaiting a foreach grammar.
        std::ostringstream os;
        bool first = true;
        walk_doc_tree(doc, [&](Curvz::SceneNode* node) -> bool {
            if (!is_scene_object(node)) return false;
            if (!first) os << ',';
            os << node->internal_id;
            first = false;
            return false;  // no early exit — full walk
        });
        return ScriptValue::text(os.str());
    }

    // find_by_name and find_by_type are invoke-shaped (take args);
    // see invoke() above. The header documents the grammar reason.

    return ScriptValue::null();
}

std::vector<std::string> ObjectsScriptable::verbs() const {
    return {
        // Read-shaped (no-arg query() can't take a string arg today;
        // exposed as invoke). Each returns iid or "".
        "find_by_name",
        "find_by_type",
        // Structural verbs (s231 m2). Each pushes a command on the
        // global undo stack on success.
        "new",
        "delete",
        // Element mutating verbs live on the proxy, not the
        // collection — see ObjectProxy::verbs(). m3 added them
        // (toggle_visible / set_visible / toggle_locked /
        // set_locked / rename). m4 will add element structural
        // verbs (move, reparent, duplicate); m5 closes with
        // collection-level grouping verbs here (group, ungroup).
    };
}

std::vector<std::string> ObjectsScriptable::properties() const {
    return {
        "count",
        "all_iids",
    };
}

} // namespace curvz::scripting
