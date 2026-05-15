// scripting/LayersScriptable.cpp ─────────────────────────────────────────────
//
// s216 m1 — implementation of the first row-bound model Scriptable.
// See LayersScriptable.hpp for the verb/query surface and lifetime
// notes; this file is the dispatch table, the private LayerProxy
// class, and the per-verb bodies.
//
// ── Why direct mutation + push-command (not command-only) ───────────────────
//
// The element-level mutating verbs (set_visible, set_locked, rename,
// set_color) follow the exact same pattern LayersPanel does:
//
//   1. Mutate the model directly (write the new value on the SceneNode).
//   2. Push an EditLayerFieldCommand with before/after.
//
// The command's execute() body re-applies the after-value on redo, and
// undo() applies the before-value. The INITIAL application is step (1) —
// `push()` does not call `execute()`. This keeps the script-driven path
// indistinguishable from the user-driven path: a real user clicking the
// eye icon takes the same two-step shape (LayersPanel's vis_btn handler
// writes `m_doc->layers[i]->visible = !before` then calls
// push_edit_layer_bool_field). The Scriptable just opens a second
// entrance to the same path.
//
// ── set_opacity and make_active — direct-only, no command ───────────────────
//
// Two verbs intentionally bypass the command system:
//
//   set_opacity: EditLayerFieldCommand's Field enum is {Name, Color,
//   Visible, Locked} — no Opacity slot today. Adding one would touch
//   CommandHistory.hpp (extend the enum) and CommandHistory.cpp
//   (extend apply_layer_field's switch). That's out of scope for the
//   pilot; the verb is shipped working-on-model so the canon surface
//   is complete, and the undo gap is documented in the header as a
//   follow-up.
//
//   make_active: changing active_layer_index isn't undoable for users
//   either (the panel mutates active_layer_index directly without
//   pushing a command — same shape we mirror). It's UI state, not
//   data state.

#include "scripting/LayersScriptable.hpp"
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

// ── ProjectGetter and friends — non-owning capture ──────────────────────────

namespace {

// Test whether a SceneNode is a "real" layer (Type::Layer). Excludes
// guide/grid/margin/ref/measure layers — those are special structural
// containers and belong to their own future Scriptables (e.g. `guides`
// for guide layers' children, etc.). The canon table is explicit on
// this: `layers | layer.<iid> | SceneNode with Type::Layer`.
bool is_real_layer(const Curvz::SceneNode* n) {
    return n && n->type == Curvz::SceneNode::Type::Layer;
}

// Pick an anchor iid from the active doc's layers — any iid that
// exists in the doc before the mutation, so the command's
// find_by_iid succeeds at apply time. Returns "" if no candidate.
// Used by structural commands (AddLayer, DeleteLayer, ReorderLayers)
// where doc identity is established via "a peer iid that survives
// the op." For Delete, callers must pass `excluded` to skip the layer
// being deleted.
std::string pick_anchor(Curvz::CurvzDocument* doc,
                        std::string_view excluded = {}) {
    if (!doc) return {};
    for (auto& l : doc->layers) {
        if (!l) continue;
        if (l->internal_id.empty()) continue;
        if (!excluded.empty() && l->internal_id == excluded) continue;
        return l->internal_id;
    }
    return {};
}

// Parse a bool from a ScriptValue. Accepts Bool directly; falls back
// to text "true"/"false" (case-sensitive — matches the DSL literal
// lexer). Returns `fallback` for anything else.
bool arg_as_bool(const ScriptValue& v, bool fallback) {
    if (v.kind == ValueKind::Bool)   return v.b;
    if (v.kind == ValueKind::String) {
        if (v.s == "true")  return true;
        if (v.s == "false") return false;
    }
    return fallback;
}

// Pull a double out of an Int/Double ScriptValue. Anything else
// returns fallback. Used by set_opacity.
double arg_as_double(const ScriptValue& v, double fallback) {
    switch (v.kind) {
        case ValueKind::Double: return v.d;
        case ValueKind::Int:    return static_cast<double>(v.i);
        default:                return fallback;
    }
}

// Resolve a string-shaped arg (quoted or token). Returns "" for any
// non-string ScriptValue, since the verbs that take strings (rename,
// set_color, move-direction, find_by_name) are unambiguous about
// wanting a string.
std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

} // anon namespace

// ── LayerProxy — transient per-instance Scriptable ──────────────────────────
//
// Materialised on demand by LayersScriptable::proxy_for(iid). Lifetime
// is bounded by the listener's ResolvedObject wrapper — destroyed when
// the wrapper goes out of scope at end-of-statement.
//
// IMPORTANT: registered via the `unregistered_t` tag — proxies must
// not appear in the global registry. Two transient proxies addressing
// the same iid in adjacent script lines would collide on the empty
// name otherwise, and `list` would either show every live proxy
// (defeating the canon's "registry namespace stays small" property)
// or need special-case filtering. The `unregistered_t` ctor exists
// for exactly this: a Scriptable that participates in the substrate
// machinery (uniform invoke/query surface, same vtable layout) but
// skips the registry — invisible to `list`, immune to name collision.
class LayerProxy : public Scriptable {
public:
    LayerProxy(LayersScriptable::ProjectGetter get_project,
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
    // Resolve our iid to a live SceneNode through the current project.
    // Returns nullptr if the iid no longer exists in any document
    // (layer deleted between addressing and verb dispatch — rare in
    // single-line scripts, but the addressability-is-current-state
    // invariant covers it).
    Curvz::SceneNode* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        return curvz::utils::find_by_iid(*proj, m_iid);
    }

    // Push an EditLayerFieldCommand with our iid. The DIRECT mutation
    // must happen at the caller site (mirrors the LayersPanel pattern:
    // mutate the SceneNode field, then push the command with
    // before/after). Skips silently if history isn't wired or proj is
    // missing — same fallback shape as the panel.
    void push_bool_field(Curvz::EditLayerFieldCommand::Field field,
                         bool before, bool after) {
        if (!m_history) return;
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return;
        if (m_iid.empty()) return;
        auto cmd = std::make_unique<Curvz::EditLayerFieldCommand>(
            proj, m_iid, field,
            /*before_str=*/std::string{}, /*after_str=*/std::string{},
            before, after);
        m_history->push(std::move(cmd));
    }
    void push_string_field(Curvz::EditLayerFieldCommand::Field field,
                           const std::string& before,
                           const std::string& after) {
        if (!m_history) return;
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return;
        if (m_iid.empty()) return;
        auto cmd = std::make_unique<Curvz::EditLayerFieldCommand>(
            proj, m_iid, field, before, after,
            /*before_bool=*/false, /*after_bool=*/false);
        m_history->push(std::move(cmd));
    }

    LayersScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*          m_history;   // non-owning; outlives us
    std::string                     m_iid;
};

// ── LayerProxy: invoke ──────────────────────────────────────────────────────

ScriptValue LayerProxy::invoke(std::string_view verb,
                               const ScriptArgs& args) {
    using Field = Curvz::EditLayerFieldCommand::Field;
    auto* layer = resolve();
    if (!layer) return ScriptValue::null();

    if (verb == "toggle_visible") {
        bool before = layer->visible;
        bool after  = !before;
        layer->visible = after;
        push_bool_field(Field::Visible, before, after);
        return ScriptValue::null();
    }

    if (verb == "set_visible") {
        if (args.empty()) return ScriptValue::null();
        bool before = layer->visible;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();  // no-op, no command
        layer->visible = after;
        push_bool_field(Field::Visible, before, after);
        return ScriptValue::null();
    }

    if (verb == "toggle_locked") {
        bool before = layer->locked;
        bool after  = !before;
        layer->locked = after;
        push_bool_field(Field::Locked, before, after);
        return ScriptValue::null();
    }

    if (verb == "set_locked") {
        if (args.empty()) return ScriptValue::null();
        bool before = layer->locked;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();
        layer->locked = after;
        push_bool_field(Field::Locked, before, after);
        return ScriptValue::null();
    }

    if (verb == "rename") {
        if (args.empty()) return ScriptValue::null();
        std::string before = layer->name;
        std::string after  = arg_as_string(args[0]);
        if (after.empty() || before == after) return ScriptValue::null();
        layer->name = after;
        push_string_field(Field::Name, before, after);
        return ScriptValue::null();
    }

    if (verb == "color" || verb == "set_color") {
        // s217 m2 — primary name is `color` (matches the inspector
        // field label). `set_color` is an alias kept for backward
        // compat through s217 m2; deprecated in CANON. Will be
        // removed in s218+ once existing scripts are swept.
        //
        // Hex strings ("#rrggbb") OR "" for "no colour tag" — the
        // canonical layer.color shape. The model accepts any string;
        // we don't validate format here (the panel doesn't either —
        // the user-facing path uses ColorPickerPopover which emits
        // valid hex by construction, and tests are responsible for
        // their own strings).
        if (args.empty()) return ScriptValue::null();
        std::string before = layer->color;
        std::string after  = arg_as_string(args[0]);
        if (before == after) return ScriptValue::null();
        layer->color = after;
        push_string_field(Field::Color, before, after);
        return ScriptValue::null();
    }

    if (verb == "opacity" || verb == "set_opacity") {
        // s217 m2 — primary name is `opacity` (matches the inspector
        // slider label). `set_opacity` is an alias kept for backward
        // compat through s217 m2; deprecated in CANON. Will be
        // removed in s218+ once existing scripts are swept.
        //
        // No undo — see header. Direct write on the model.
        if (args.empty()) return ScriptValue::null();
        double v = arg_as_double(args[0], layer->opacity);
        // Clamp to [0, 1] — the inspector's spinner clamps the same
        // way, so the model never sees out-of-range values from the
        // user-driven path; mirror that here.
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        layer->opacity = v;
        return ScriptValue::null();
    }

    if (verb == "activate" || verb == "make_active") {
        // s217 m2 — primary name is `activate` (the user verb — "click
        // the row to make it active"). `make_active` is an alias kept
        // for backward compat through s217 m2; deprecated in CANON.
        // Will be removed in s218+ once existing scripts are swept.
        //
        // Find which doc and index this layer lives at. Walking the
        // project (rather than just the active doc) means activate
        // can promote a layer in a non-active doc to that doc's active
        // layer — useful for tests that prep state across docs. The
        // doc identity comes from find_doc_by_iid; the index from
        // find_layer_index_by_iid (both pumps live in curvz_utils, both
        // doc-local).
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return ScriptValue::null();
        auto* doc = curvz::utils::find_doc_by_iid(*proj, m_iid);
        if (!doc) return ScriptValue::null();
        int idx = curvz::utils::find_layer_index_by_iid(*doc, m_iid);
        if (idx < 0) return ScriptValue::null();
        doc->active_layer_index = idx;
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── LayerProxy: query ───────────────────────────────────────────────────────

ScriptValue LayerProxy::query(std::string_view property) const {
    auto* layer = resolve();
    if (!layer) return ScriptValue::null();

    if (property == "name")        return ScriptValue::text(layer->name);
    if (property == "visible")     return ScriptValue::boolean(layer->visible);
    if (property == "locked")      return ScriptValue::boolean(layer->locked);
    if (property == "opacity")     return ScriptValue::real(layer->opacity);
    if (property == "color")       return ScriptValue::text(layer->color);
    if (property == "child_count") return ScriptValue::integer(
        static_cast<long long>(layer->children.size()));
    if (property == "iid")         return ScriptValue::text(layer->internal_id);

    return ScriptValue::null();
}

std::vector<std::string> LayerProxy::verbs() const {
    // s217 m2 — primary verb names. `color`, `opacity`, `activate`
    // replaced `set_color`, `set_opacity`, `make_active`. The aliases
    // are still accepted by invoke() but intentionally NOT listed
    // here: discoverability should push new script authors toward the
    // canonical UI-vocabulary names. Existing scripts using the old
    // names keep working through s217 m2; CANON deprecates them and
    // s218+ removes the alias branches.
    return {
        "toggle_visible",
        "set_visible",
        "toggle_locked",
        "set_locked",
        "rename",
        "color",
        "opacity",
        "activate",
    };
}

std::vector<std::string> LayerProxy::properties() const {
    return {
        "name", "visible", "locked", "opacity", "color",
        "child_count", "iid",
    };
}

// ── LayersScriptable ────────────────────────────────────────────────────────

LayersScriptable::LayersScriptable(ProjectGetter get_project,
                                   Curvz::CommandHistory* history)
    : Scriptable("layers")
    , m_get_project(std::move(get_project))
    , m_history(history) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "layers". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime.
}

// ── Router hooks ────────────────────────────────────────────────────────────

bool LayersScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    auto* node = curvz::utils::find_by_iid(*proj, std::string(key));
    return is_real_layer(node);
}

std::unique_ptr<Scriptable>
LayersScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    // The proxy needs both the project getter (for cross-doc iid
    // resolution) and the history pointer (for command pushing on
    // mutating verbs). Both are stable across the proxy's lifetime
    // — the project getter resolves dynamically, the history pointer
    // is a stable-address member of MainWindow. nullptr history is
    // tolerated: LayerProxy::push_* checks before pushing, so a test
    // harness without history wired still gets correct model
    // mutations, just no undo entries.
    return std::make_unique<LayerProxy>(m_get_project, m_history,
                                        std::string(key));
}

// ── Collection invoke ───────────────────────────────────────────────────────

ScriptValue LayersScriptable::invoke(std::string_view verb,
                                     const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto* doc = proj->active_doc();
    if (!doc) return ScriptValue::null();

    if (verb == "new") {
        // Mirror LayersPanel::on_add_layer's shape — same insertion
        // policy (front of vector but after any special layer at
        // position 0), same name uniquifier, same palette colour
        // assignment, same command push.
        auto layer = std::make_unique<Curvz::SceneNode>();
        layer->type = Curvz::SceneNode::Type::Layer;
        layer->internal_id = Curvz::generate_internal_id();
        layer->name = doc->next_default_name(
            Curvz::CurvzDocument::NameKind::Layer);
        // No palette colour — the LayersPanel ctor's PALETTE_SIZE is
        // a per-panel constant, not a model concern. Scripts that
        // want a colour set it explicitly via `layers.<iid> set_color`.
        // Leaving layer->color empty falls through to the panel's
        // "no colour tag" rendering, the same as a fresh project's
        // first layer.

        int insert_pos = 0;
        for (int i = 0; i < (int)doc->layers.size(); ++i) {
            auto& l = doc->layers[i];
            if (!l) continue;
            if (!l->is_guide_layer() && !l->is_ref_layer() &&
                !l->is_measure_layer() && !l->is_grid_layer() &&
                !l->is_margin_layer()) {
                insert_pos = i;
                break;
            }
        }

        int active_before = doc->active_layer_index;
        int active_after  = insert_pos;
        std::string new_iid = layer->internal_id;
        std::string anchor_iid = pick_anchor(doc);

        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history && !anchor_iid.empty()) {
            snap_for_cmd = Curvz::clone_node(*layer);
        }

        doc->layers.insert(doc->layers.begin() + insert_pos,
                           std::move(layer));
        doc->invalidate_iid_index();
        doc->active_layer_index = insert_pos;

        if (snap_for_cmd) {
            auto cmd = std::make_unique<Curvz::AddLayerCommand>(
                proj, anchor_iid, std::move(snap_for_cmd),
                insert_pos, active_before, active_after);
            m_history->push(std::move(cmd));
        }
        return ScriptValue::text(new_iid);
    }

    if (verb == "delete") {
        // Mirror LayersPanel::on_delete_layer's invariants: never
        // delete a special-layer slot; keep at least one real layer.
        if (args.empty()) return ScriptValue::null();
        std::string target_iid = arg_as_string(args[0]);
        if (target_iid.empty()) return ScriptValue::null();
        // find_doc_by_iid + find_layer_index_by_iid — delete only
        // touches the doc the iid actually lives in, NOT necessarily
        // the active doc. This is intentional: a script targeting a
        // specific iid says "delete THAT layer", and the doc identity
        // is encoded in the iid's resolution.
        auto* target_doc = curvz::utils::find_doc_by_iid(*proj, target_iid);
        if (!target_doc) return ScriptValue::null();
        int idx = curvz::utils::find_layer_index_by_iid(*target_doc, target_iid);
        if (idx < 0) return ScriptValue::null();
        auto& slot = target_doc->layers[idx];
        if (!slot || !is_real_layer(slot.get())) return ScriptValue::null();
        // At-least-one-real-layer invariant.
        int real_count = 0;
        for (auto& l : target_doc->layers)
            if (is_real_layer(l.get())) ++real_count;
        if (real_count <= 1) return ScriptValue::null();

        int active_before = target_doc->active_layer_index;
        // Anchor must be a SURVIVING peer — exclude the layer about
        // to be deleted.
        std::string anchor_iid = pick_anchor(target_doc, target_iid);

        std::unique_ptr<Curvz::SceneNode> snap_for_cmd;
        if (m_history && !anchor_iid.empty()) {
            snap_for_cmd = Curvz::clone_node(*slot);
        }

        // No raw-pointer-capture scrub here: the LayersPanel does it
        // because pre-s171 raw-pointer commands may exist on the
        // stack and reference children of the layer being deleted.
        // The Scriptable is a new code path; any commands it pushes
        // are iid-based. But the GLOBAL undo stack can still hold
        // legacy raw-pointer commands from any other code path, so
        // we run the same scrub_walk the panel does — defensive and
        // cheap. Skips cleanly if m_history is null.
        if (m_history) {
            Curvz::SceneNode* to_delete = slot.get();
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

        target_doc->layers.erase(target_doc->layers.begin() + idx);
        target_doc->invalidate_iid_index();
        int new_active = std::min(active_before,
                                  (int)target_doc->layers.size() - 1);
        while (new_active >= 0 &&
               !is_real_layer(target_doc->layers[new_active].get())) {
            --new_active;
        }
        if (new_active < 0) new_active = 0;
        target_doc->active_layer_index = new_active;

        if (snap_for_cmd) {
            auto cmd = std::make_unique<Curvz::DeleteLayerCommand>(
                proj, anchor_iid, std::move(snap_for_cmd),
                idx, active_before, new_active);
            m_history->push(std::move(cmd));
        }
        return ScriptValue::null();
    }

    if (verb == "move") {
        // Two args: iid (string), direction (string: up|down|top|bottom).
        // Reorder is whole-vector permutation; we use ReorderLayersCommand
        // which captures iids_before/iids_after and replays via re-sort.
        if (args.size() < 2) return ScriptValue::null();
        std::string target_iid = arg_as_string(args[0]);
        std::string dir        = arg_as_string(args[1]);
        if (target_iid.empty() || dir.empty()) return ScriptValue::null();

        auto* target_doc = curvz::utils::find_doc_by_iid(*proj, target_iid);
        if (!target_doc) return ScriptValue::null();
        int idx = curvz::utils::find_layer_index_by_iid(*target_doc, target_iid);
        if (idx < 0) return ScriptValue::null();

        // Snapshot iids before.
        std::vector<std::string> iids_before;
        iids_before.reserve(target_doc->layers.size());
        for (auto& l : target_doc->layers)
            iids_before.push_back(l ? l->internal_id : std::string{});

        // Compute target index. Special layers can be anywhere in
        // the vector but real-layer movement should stay within
        // sensible bounds; for the pilot we move only the iid we
        // were given and clamp at vector ends. This matches what a
        // DnD reorder would produce.
        int new_idx = idx;
        if      (dir == "up")     new_idx = std::max(0, idx - 1);
        else if (dir == "down")   new_idx = std::min((int)target_doc->layers.size() - 1, idx + 1);
        else if (dir == "top")    new_idx = 0;
        else if (dir == "bottom") new_idx = (int)target_doc->layers.size() - 1;
        else return ScriptValue::null();

        if (new_idx == idx) return ScriptValue::null();  // no-op

        auto layer = std::move(target_doc->layers[idx]);
        target_doc->layers.erase(target_doc->layers.begin() + idx);
        target_doc->layers.insert(target_doc->layers.begin() + new_idx,
                                  std::move(layer));
        target_doc->invalidate_iid_index();

        // active_layer_index follows the moved layer if it WAS the
        // active one; otherwise stays the same (the index now refers
        // to a different layer in the reordered vector). Both shapes
        // match what DnD does in LayersPanel.
        int active_before = target_doc->active_layer_index;
        int active_after  = active_before;
        if (active_before == idx) active_after = new_idx;

        target_doc->active_layer_index = active_after;

        std::vector<std::string> iids_after;
        iids_after.reserve(target_doc->layers.size());
        for (auto& l : target_doc->layers)
            iids_after.push_back(l ? l->internal_id : std::string{});

        if (m_history) {
            std::string anchor_iid;
            for (auto& s : iids_after) {
                if (!s.empty()) { anchor_iid = s; break; }
            }
            if (!anchor_iid.empty()) {
                auto cmd = std::make_unique<Curvz::ReorderLayersCommand>(
                    proj, anchor_iid,
                    std::move(iids_before), std::move(iids_after),
                    active_before, active_after);
                m_history->push(std::move(cmd));
            }
        }
        return ScriptValue::null();
    }

    if (verb == "hide_others") {
        // Set visible=false on every real layer EXCEPT the one named
        // by the iid arg. Pushes one EditLayerFieldCommand per layer
        // that actually flipped — chatty for undo (each hide is its
        // own step) but correct and predictable.
        if (args.empty()) return ScriptValue::null();
        std::string keep_iid = arg_as_string(args[0]);
        if (keep_iid.empty()) return ScriptValue::null();

        using Field = Curvz::EditLayerFieldCommand::Field;
        for (auto& l : doc->layers) {
            if (!is_real_layer(l.get())) continue;
            if (l->internal_id == keep_iid) continue;
            if (!l->visible) continue;  // already hidden, skip
            bool before = l->visible;
            l->visible = false;
            if (m_history && !l->internal_id.empty()) {
                auto cmd = std::make_unique<Curvz::EditLayerFieldCommand>(
                    proj, l->internal_id, Field::Visible,
                    std::string{}, std::string{},
                    before, false);
                m_history->push(std::move(cmd));
            }
        }
        return ScriptValue::null();
    }

    if (verb == "show_all") {
        using Field = Curvz::EditLayerFieldCommand::Field;
        for (auto& l : doc->layers) {
            if (!is_real_layer(l.get())) continue;
            if (l->visible) continue;
            bool before = l->visible;
            l->visible = true;
            if (m_history && !l->internal_id.empty()) {
                auto cmd = std::make_unique<Curvz::EditLayerFieldCommand>(
                    proj, l->internal_id, Field::Visible,
                    std::string{}, std::string{},
                    before, true);
                m_history->push(std::move(cmd));
            }
        }
        return ScriptValue::null();
    }

    if (verb == "find_by_name") {
        // Read-shaped verb — returns iid of first Type::Layer with the
        // matching name, or "" on miss. find_by_name is invoke-shaped
        // (not query-shaped) because today's query() can't take args:
        // `get layers find_by_name "X"` would put "find_by_name" in
        // the property slot and "X" would be ignored. Exposing it as
        // an invoke verb means scripts write `layers find_by_name "X"`
        // which falls through to the default invoke dispatcher and
        // gets the result through the `=` output line. Future grammar:
        // when query() grows args, this can graduate to a real query.
        if (args.empty()) return ScriptValue::text("");
        std::string target = arg_as_string(args[0]);
        if (target.empty()) return ScriptValue::text("");
        for (auto& l : doc->layers) {
            if (!is_real_layer(l.get())) continue;
            if (l->name == target) return ScriptValue::text(l->internal_id);
        }
        return ScriptValue::text("");
    }

    return ScriptValue::null();
}

ScriptValue LayersScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — return defensible empty values rather than null
        // for the count/iid queries, so a test running before project
        // open sees `0` / `""` (predictable) not `null` (an error
        // shape).
        if (property == "count")      return ScriptValue::integer(0);
        if (property == "all_iids")   return ScriptValue::text("");
        if (property == "active_iid") return ScriptValue::text("");
        return ScriptValue::null();
    }
    auto* doc = proj->active_doc();
    if (!doc) {
        if (property == "count")      return ScriptValue::integer(0);
        if (property == "all_iids")   return ScriptValue::text("");
        if (property == "active_iid") return ScriptValue::text("");
        return ScriptValue::null();
    }

    if (property == "count") {
        long long n = 0;
        for (auto& l : doc->layers) if (is_real_layer(l.get())) ++n;
        return ScriptValue::integer(n);
    }

    if (property == "all_iids") {
        // Top-down order: the LayersPanel renders index 0 at the top,
        // so doc->layers[0]'s iid comes first. Comma-separated; empty
        // string if zero real layers. The canon's note about ScriptValue
        // having no array type — this is the substring shape that fills
        // the gap until a future foreach grammar lands.
        std::ostringstream os;
        bool first = true;
        for (auto& l : doc->layers) {
            if (!is_real_layer(l.get())) continue;
            if (!first) os << ',';
            os << l->internal_id;
            first = false;
        }
        return ScriptValue::text(os.str());
    }

    if (property == "active_iid") {
        auto* active = doc->active_layer();
        if (!active) return ScriptValue::text("");
        return ScriptValue::text(active->internal_id);
    }

    // find_by_name is invoke-shaped (takes an arg), so it can't be a
    // query in today's grammar — `get layers find_by_name "Bg"` would
    // hit the 3-token assert/get parser and "find_by_name" would be
    // the property. We expose it as an INVOKE verb on the collection
    // instead (handled in invoke()), even though semantically it's a
    // pure read. The test uses `layers find_by_name "X"` which falls
    // through to the default-invoke path. Future grammar extension:
    // when query() grows an args parameter, this can move into the
    // query branch and `get layers find_by_name "X"` becomes legal.

    return ScriptValue::null();
}

std::vector<std::string> LayersScriptable::verbs() const {
    return {
        // Structural — push commands.
        "new",
        "delete",
        "move",
        "hide_others",
        "show_all",
        // Read-shaped (no-arg query() can't take a string arg today;
        // exposed as invoke). Returns iid or "".
        "find_by_name",
    };
}

std::vector<std::string> LayersScriptable::properties() const {
    return {
        "count",
        "all_iids",
        "active_iid",
    };
}

} // namespace curvz::scripting
