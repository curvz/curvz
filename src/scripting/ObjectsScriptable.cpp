// scripting/ObjectsScriptable.cpp ────────────────────────────────────────────
//
// s230 m1 — implementation of the sixth row-bound model Scriptable.
// See ObjectsScriptable.hpp for the verb/query surface, lifetime
// notes, the scope choice (real scene contents only — option (ii)
// from the s230 handoff's design fork), and the "READ-SIDE only"
// scope of m1. This file is the dispatch table, the private
// ObjectProxy class, and the per-query bodies.
//
// ── No commands pushed in m1 ─────────────────────────────────────────────
//
// Every body in this file is read-only. There are no EditObjectCommand
// pushes because no verb writes — m1 opens the addressing surface and
// validates it via the read side. The m_history pointer is captured
// for shape-symmetry with the five shipped row-bound Scriptables but
// never dereferenced today.
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
// m1 ships READ-SIDE only. invoke() returns null for any input;
// m3+ wires it to mutating verb bodies that push EditObjectCommand-
// family commands.
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

    ObjectsScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*           m_history;   // captured but unused
                                                  // in m1; see header
    std::string                      m_iid;
};

// ── ObjectProxy: invoke ──────────────────────────────────────────────────
//
// No verbs in m1 — read-side only. Every dispatch returns null. The
// signature is present (and not stripped) so future milestones can
// drop in verb bodies without touching the class shape.

ScriptValue ObjectProxy::invoke(std::string_view /*verb*/,
                                const ScriptArgs& /*args*/) {
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
    // No verbs in m1 — see invoke() rationale above.
    return {};
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
// m1 has no MUTATING verbs but does have two read-shaped verbs
// (find_by_name, find_by_type) — they live here because today's
// query() can't take args. See header note "find_by_name and
// find_by_type are invoke-shaped" above.

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
        // No mutating verbs in m1 — m3+ adds the EditObjectCommand-
        // backed verbs (toggle_visible, set_visible, set_locked,
        // rename). m4+ adds structural verbs (move, reparent, delete,
        // duplicate). m5+ adds collection-level structural verbs
        // (new <type>, group, ungroup).
    };
}

std::vector<std::string> ObjectsScriptable::properties() const {
    return {
        "count",
        "all_iids",
    };
}

} // namespace curvz::scripting
