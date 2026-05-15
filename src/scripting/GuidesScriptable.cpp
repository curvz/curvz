// scripting/GuidesScriptable.cpp ─────────────────────────────────────────────
//
// s218 m1 — implementation of the second row-bound model Scriptable.
// See GuidesScriptable.hpp for the verb/query surface, lifetime notes,
// and the "guides aren't undoable" design call. This file is the
// dispatch table, the private GuideProxy class, and the per-verb
// bodies.
//
// ── No command pushes ──────────────────────────────────────────────────────
//
// Every mutating verb in this file is direct-write on the model. There
// are no EditGuideFieldCommand pushes because no such command exists —
// the PropertiesPanel guide section, Canvas::begin_guide_drag, and the
// "From 2 points…" handler all mutate guides directly without command
// participation. The Scriptable mirrors that exact shape; a future
// session that introduces guide-undoability (Field enum on a new
// command, apply switch) can sweep this file to add push sites.
//
// The m_history pointer is captured by the proxy ctor for shape-
// symmetry with LayerProxy but never dereferenced today.

#include "scripting/GuidesScriptable.hpp"
#include "scripting/Scriptable.hpp"

#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "SceneNode.hpp"
#include "color/Color.hpp"
#include "curvz_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

// ── Argument coercion helpers ──────────────────────────────────────────────
//
// Same shape as the equivalent block in LayersScriptable.cpp. Kept
// duplicated rather than hoisted to a shared header because the
// coercion is small, file-local, and folding it to a shared site would
// add a header dependency for what amounts to two-line helpers.

namespace {

bool is_guide_node(const Curvz::SceneNode* n) {
    return n && n->type == Curvz::SceneNode::Type::Guide;
}

bool arg_as_bool(const ScriptValue& v, bool fallback) {
    if (v.kind == ValueKind::Bool)   return v.b;
    if (v.kind == ValueKind::String) {
        if (v.s == "true")  return true;
        if (v.s == "false") return false;
    }
    return fallback;
}

double arg_as_double(const ScriptValue& v, double fallback) {
    switch (v.kind) {
        case ValueKind::Double: return v.d;
        case ValueKind::Int:    return static_cast<double>(v.i);
        default:                return fallback;
    }
}

std::string arg_as_string(const ScriptValue& v) {
    if (v.kind == ValueKind::String) return v.s;
    return {};
}

// Format the doc-level guide colour as a "#rrggbb" string. Mirrors the
// fixed-precision approach used elsewhere (no alpha — the guide colour
// is opaque by construction; the inspector's swatch ignores alpha too).
std::string doc_guide_color_as_hex(const Curvz::CurvzDocument& doc) {
    auto clamp01 = [](double v) {
        if (v < 0.0) return 0.0;
        if (v > 1.0) return 1.0;
        return v;
    };
    int r = static_cast<int>(std::lround(clamp01(doc.guide_color_r) * 255.0));
    int g = static_cast<int>(std::lround(clamp01(doc.guide_color_g) * 255.0));
    int b = static_cast<int>(std::lround(clamp01(doc.guide_color_b) * 255.0));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return std::string(buf);
}

} // anon namespace

// ── GuideProxy — transient per-instance Scriptable ─────────────────────────
//
// Same lifetime contract as LayerProxy (see s216 m1 / s217 m2 design
// notes in LayersScriptable.cpp): materialised by
// GuidesScriptable::proxy_for(iid), destroyed when the listener's
// ResolvedObject wrapper goes out of scope at end-of-statement.
// Registered via the `unregistered` tag so proxies are invisible to
// the global registry — `list` shows `guides` exactly once regardless
// of how many proxies are materialised across script runs.
class GuideProxy : public Scriptable {
public:
    GuideProxy(GuidesScriptable::ProjectGetter get_project,
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
    // Returns nullptr if the iid no longer addresses a Type::Guide node
    // (deleted, or somehow re-typed). Filters on is_guide_node so a
    // stale iid that happens to collide with a non-guide returns null
    // — same defensive shape as LayerProxy::resolve's is_real_layer
    // filter.
    Curvz::SceneNode* resolve() const {
        auto* proj = m_get_project ? m_get_project() : nullptr;
        if (!proj) return nullptr;
        auto* n = curvz::utils::find_by_iid(*proj, m_iid);
        return is_guide_node(n) ? n : nullptr;
    }

    GuidesScriptable::ProjectGetter m_get_project;
    Curvz::CommandHistory*          m_history;   // captured but unused
                                                 // today; see header
    std::string                     m_iid;
};

// ── GuideProxy: invoke ─────────────────────────────────────────────────────

ScriptValue GuideProxy::invoke(std::string_view verb,
                               const ScriptArgs& args) {
    auto* guide = resolve();
    if (!guide) return ScriptValue::null();

    if (verb == "toggle_locked") {
        guide->locked = !guide->locked;
        return ScriptValue::null();
    }

    if (verb == "set_locked") {
        if (args.empty()) return ScriptValue::null();
        bool before = guide->locked;
        bool after  = arg_as_bool(args[0], before);
        if (before == after) return ScriptValue::null();  // no-op
        guide->locked = after;
        return ScriptValue::null();
    }

    if (verb == "x") {
        // Direct write — the inspector's POS-X spinner does the same
        // (g->guide_x = v; emit_prop_changed; no command). The
        // Scriptable path omits the redraw-kick since the canvas-side
        // doc_changed listener path covers script-driven mutations
        // when m_sig_doc_changed fires on subsequent operations. If
        // a script wants to see the canvas update mid-run, it can
        // pause for a frame via `sleep 16` or trigger a paint via a
        // structural verb like `new` / `delete`.
        if (args.empty()) return ScriptValue::null();
        guide->guide_x = arg_as_double(args[0], guide->guide_x);
        return ScriptValue::null();
    }

    if (verb == "y") {
        if (args.empty()) return ScriptValue::null();
        guide->guide_y = arg_as_double(args[0], guide->guide_y);
        return ScriptValue::null();
    }

    if (verb == "angle") {
        // Degrees, same convention as the model: 0=horizontal,
        // 90=vertical. The inspector's angle spinner doesn't
        // normalize; we don't either. Callers that want a specific
        // range can wrap with modulo themselves.
        if (args.empty()) return ScriptValue::null();
        guide->guide_angle = arg_as_double(args[0], guide->guide_angle);
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── GuideProxy: query ──────────────────────────────────────────────────────

ScriptValue GuideProxy::query(std::string_view property) const {
    auto* guide = resolve();
    if (!guide) return ScriptValue::null();

    if (property == "locked") return ScriptValue::boolean(guide->locked);
    if (property == "x")      return ScriptValue::real(guide->guide_x);
    if (property == "y")      return ScriptValue::real(guide->guide_y);
    if (property == "angle")  return ScriptValue::real(guide->guide_angle);
    if (property == "iid")    return ScriptValue::text(guide->internal_id);

    if (property == "type") {
        // Mirror the inspector's H/V/A type label, case-lowered for
        // script-stable lookups (the panel shows uppercase as visual
        // emphasis; scripts comparing against literals are easier when
        // the case convention is fixed and lowercase). Same axis-aligned
        // tests SceneNode::guide_is_horizontal / _vertical use.
        if (guide->guide_is_horizontal()) return ScriptValue::text("h");
        if (guide->guide_is_vertical())   return ScriptValue::text("v");
        return ScriptValue::text("a");
    }

    return ScriptValue::null();
}

std::vector<std::string> GuideProxy::verbs() const {
    return {
        "toggle_locked",
        "set_locked",
        "x",
        "y",
        "angle",
    };
}

std::vector<std::string> GuideProxy::properties() const {
    return {
        "locked", "x", "y", "angle", "type", "iid",
    };
}

// ── GuidesScriptable ───────────────────────────────────────────────────────

GuidesScriptable::GuidesScriptable(ProjectGetter get_project,
                                   Curvz::CommandHistory* history)
    : Scriptable("guides")
    , m_get_project(std::move(get_project))
    , m_history(history) {
    // Registry registration happens in the Scriptable base ctor under
    // the name "guides". MainWindow holds us as a member; the registry
    // entry lives for the window's lifetime.
}

// ── Router hooks ───────────────────────────────────────────────────────────

bool GuidesScriptable::can_resolve(std::string_view key) const {
    if (key.empty()) return false;
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return false;
    auto* node = curvz::utils::find_by_iid(*proj, std::string(key));
    return is_guide_node(node);
}

std::unique_ptr<Scriptable>
GuidesScriptable::proxy_for(std::string_view key) {
    if (!can_resolve(key)) return nullptr;
    return std::make_unique<GuideProxy>(m_get_project, m_history,
                                        std::string(key));
}

// ── Collection invoke ──────────────────────────────────────────────────────

ScriptValue GuidesScriptable::invoke(std::string_view verb,
                                     const ScriptArgs& args) {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) return ScriptValue::null();
    auto* doc = proj->active_doc();
    if (!doc) return ScriptValue::null();

    if (verb == "new") {
        // Create a Type::Guide node parked as a child of the doc's
        // guide layer. Mirrors the model side of Canvas::begin_guide_drag
        // and the "From 2 points…" handler — straight push_back into
        // gl->children, no command (guides aren't undoable, see header).
        //
        // Default geometry: centred horizontal at (canvas_w/2,
        // canvas_h/2) angle 0. Subsequent x / y / angle proxy verbs
        // shape it. We invalidate the iid index after the push so the
        // next-line `guides.<iid>` lookup hits without falling through
        // to the self-heal path.
        Curvz::SceneNode* gl = doc->ensure_guide_layer();
        if (!gl) return ScriptValue::null();
        if (gl->locked) return ScriptValue::null();  // mirror the inspector
                                                     // guard — no creation
                                                     // into a locked layer.
        if (!gl->visible) gl->visible = true;        // creating a guide
                                                     // implies the user
                                                     // wants to see them
                                                     // (matches the canvas
                                                     // ruler-drag path).

        auto guide = std::make_unique<Curvz::SceneNode>();
        guide->type = Curvz::SceneNode::Type::Guide;
        // internal_id is default-initialised by SceneNode's struct-level
        // = generate_internal_id() (s168 m2). No explicit assignment
        // needed; clone_node and SvgParser are the only sites that
        // overwrite it.
        guide->guide_x = doc->canvas_width() * 0.5;
        guide->guide_y = doc->canvas_height() * 0.5;
        guide->guide_angle = 0.0;

        std::string new_iid = guide->internal_id;
        gl->children.push_back(std::move(guide));
        doc->invalidate_iid_index();
        return ScriptValue::text(new_iid);
    }

    if (verb == "delete") {
        // Find which doc actually holds the iid (cross-doc resolution,
        // same shape as LayersScriptable::invoke's delete branch).
        // Walk the doc's guide layer children, erase the matching slot.
        // No command push — guides aren't undoable.
        if (args.empty()) return ScriptValue::null();
        std::string target_iid = arg_as_string(args[0]);
        if (target_iid.empty()) return ScriptValue::null();

        auto* target_doc = curvz::utils::find_doc_by_iid(*proj, target_iid);
        if (!target_doc) return ScriptValue::null();
        Curvz::SceneNode* gl = target_doc->guide_layer();
        if (!gl) return ScriptValue::null();

        bool erased = false;
        gl->children.erase(
            std::remove_if(gl->children.begin(), gl->children.end(),
                           [&](const std::unique_ptr<Curvz::SceneNode>& c) {
                               if (c && c->internal_id == target_iid &&
                                   is_guide_node(c.get())) {
                                   erased = true;
                                   return true;
                               }
                               return false;
                           }),
            gl->children.end());
        if (erased) target_doc->invalidate_iid_index();
        return ScriptValue::null();
    }

    if (verb == "color") {
        // Doc-level guide colour — writes doc->guide_color_r/g/b from
        // a hex string. Mirrors the Color row in
        // build_object_guides_section, which edits doc->guide_color_*
        // directly via ColorPickerPopover. No undo (the inspector
        // doesn't push for this edit either; the picker write is
        // direct).
        //
        // Empty string is intentionally NOT a "clear to default" verb
        // here — the inspector has no such affordance, and adding one
        // would be inventing UI vocabulary. Empty / unparseable input
        // is silently rejected (no-op), matching how a hex-parse
        // failure in the picker would degrade.
        if (args.empty()) return ScriptValue::null();
        std::string hex = arg_as_string(args[0]);
        if (hex.empty()) return ScriptValue::null();
        auto parsed = Curvz::color::from_hex(hex);
        if (!parsed) return ScriptValue::null();
        doc->guide_color_r = parsed->r;
        doc->guide_color_g = parsed->g;
        doc->guide_color_b = parsed->b;
        return ScriptValue::null();
    }

    return ScriptValue::null();
}

// ── Collection query ───────────────────────────────────────────────────────

ScriptValue GuidesScriptable::query(std::string_view property) const {
    auto* proj = m_get_project ? m_get_project() : nullptr;
    if (!proj) {
        // No project — defensible empties (see LayersScriptable::query
        // for the same shape; a test running before project open sees
        // 0 / "" not null).
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_iids") return ScriptValue::text("");
        if (property == "color")    return ScriptValue::text("");
        return ScriptValue::null();
    }
    auto* doc = proj->active_doc();
    if (!doc) {
        if (property == "count")    return ScriptValue::integer(0);
        if (property == "all_iids") return ScriptValue::text("");
        if (property == "color")    return ScriptValue::text("");
        return ScriptValue::null();
    }

    Curvz::SceneNode* gl = doc->guide_layer();  // may be null if no
                                                // guide layer exists
                                                // in this doc yet

    if (property == "count") {
        if (!gl) return ScriptValue::integer(0);
        long long n = 0;
        for (auto& c : gl->children)
            if (is_guide_node(c.get())) ++n;
        return ScriptValue::integer(n);
    }

    if (property == "all_iids") {
        if (!gl) return ScriptValue::text("");
        std::ostringstream os;
        bool first = true;
        for (auto& c : gl->children) {
            if (!is_guide_node(c.get())) continue;
            if (!first) os << ',';
            os << c->internal_id;
            first = false;
        }
        return ScriptValue::text(os.str());
    }

    if (property == "color") {
        return ScriptValue::text(doc_guide_color_as_hex(*doc));
    }

    return ScriptValue::null();
}

std::vector<std::string> GuidesScriptable::verbs() const {
    return {
        "new",
        "delete",
        "color",
    };
}

std::vector<std::string> GuidesScriptable::properties() const {
    return {
        "count",
        "all_iids",
        "color",
    };
}

} // namespace curvz::scripting
