//
// StyleInterop.cpp — funnel implementation.
//
// mutate_appearance is live (since S74 m2). materialise_from_style is
// live as of S75 m3a — it projects Style::fill onto node.fill and
// Style::stroke (a StrokeAppearance) onto node.stroke (a StrokeStyle),
// using the existing Paint→FillStyle bridge for the paint variants.
//
// The library-less to_fillstyle overload is used intentionally: a Style's
// fill can be a SwatchRef, and resolving it via to_fillstyle(p, lib)
// would need the SwatchLibrary threaded through this signature too. The
// library-less path degrades SwatchRef to its cached fallback, which is
// always a visible colour (refreshed on every set_paint and on live
// recolour), so the materialised appearance is correct for render
// purposes. The SwatchRef id is carried separately onto node.fill_swatch_id
// / stroke_swatch_id so live_recolour_walk still finds the binding when
// the underlying swatch is later edited — same pattern SwatchLibrary::
// set_paint uses to cache-plus-identify.
//
// StrokeAppearance→StrokeStyle field mapping (m3a):
//   paint         → stroke.paint           (via to_fillstyle)
//   width         → stroke.width
//   cap           → stroke.cap             (same LineCap enum)
//   join          → stroke.join            (same LineJoin enum)
//   miter_limit   → stroke.miter
//   -             → stroke.opacity is NOT touched; StrokeAppearance has
//                   no opacity concept (it lives on the SceneNode), so
//                   the existing opacity on the node survives materialise.
//
// S87 m1: dash, dash_offset, and stroke alignment were removed from
// StrokeAppearance entirely (they had been specced for a future phase
// and previously DROPPED here on the way to StrokeStyle). Mapping is
// now a clean 1:1 between StrokeAppearance and StrokeStyle.
//
// S98: ShadowAppearance projection lands here too. Field mapping is 1:1
// onto SceneNode's loose shadow_* fields (the type lives separately for
// style-vs-node-layer hygiene, not because the shapes diverge). The
// projection ALWAYS runs — including when shadow.enabled is false — so
// binding a "no shadow" style turns off any pre-existing per-object
// shadow on the host. The style is the appearance, full stop.

#include "style/StyleInterop.hpp"
#include "style/StyleLibrary.hpp"

#include "SceneNode.hpp"
#include "color/FillStyleInterop.hpp"  // to_fillstyle (Paint → FillStyle)
#include "color/Paint.hpp"             // std::get_if<SwatchRef>
#include "CurvzLog.hpp"

namespace Curvz {
namespace style {

namespace {

// Extract the SwatchRef id from a Paint, or empty if it's not a ref.
// Mirrors the pattern in SwatchLibrary::set_paint — we need the id
// separately from the resolved FillStyle so the node's *_swatch_id
// binding cache stays populated through materialise. Without this,
// live_recolour_walk wouldn't find the node after a swatch edit.
std::string swatch_id_of(const color::Paint& p) {
    if (const auto* sr = std::get_if<color::SwatchRef>(&p)) {
        return sr->id;
    }
    return {};
}

} // namespace

void mutate_appearance(SceneNode& node,
                       const std::function<void(SceneNode&)>& fn) {
    // Step 1: break the binding if there is one. Per addendum:
    // "Override breaks the binding. Direct edits to a bound object's
    // fill or stroke clear bound_style and freeze the object at its
    // current appearance." The freezing is automatic — fill / stroke
    // are already a cache of the style, so they're already correct
    // for the moment-of-override.
    if (!node.bound_style.empty()) {
        LOG_TRACE("style::mutate_appearance: breaking binding to '{}'",
                  node.bound_style);
        node.bound_style.clear();
    }

    // Step 1b (S82 m4f): break swatch bindings on the same override-unbinds
    // principle. A direct user write to fill / stroke means the user is no
    // longer tracking the swatch — the swatch_id field becomes stale and
    // would mislead live_recolour_walk into recolouring an object whose
    // colour the user just hand-edited. Symmetric with the bound_style
    // break above; mirrors break-on-override across both binding kinds.
    //
    // Order matters w.r.t. fn: clear FIRST so set_paint's fn (which sets
    // the new id) lands on the empty slot. set_paint is the ONE caller
    // whose fn intentionally re-populates the id; every other funnel
    // caller leaves the id empty post-mutation, which is the correct
    // override-unbinds outcome.
    if (!node.fill_swatch_id.empty()) {
        LOG_TRACE("style::mutate_appearance: breaking fill swatch binding "
                  "to '{}'", node.fill_swatch_id);
        node.fill_swatch_id.clear();
    }
    if (!node.stroke_swatch_id.empty()) {
        LOG_TRACE("style::mutate_appearance: breaking stroke swatch binding "
                  "to '{}'", node.stroke_swatch_id);
        node.stroke_swatch_id.clear();
    }

    // Step 2: run the mutator. Defensive null-check on the std::function
    // target — callers of mutate_appearance may pass {} if all they want
    // is the unbind-side-effect (rare but legitimate; the inspector's
    // explicit "Unbind" button uses UnbindStyleCommand, not this path,
    // but a future call site might).
    if (fn) {
        fn(node);
    }

    // S106 m1 — fill content may have changed (gradient stops, type
    // swap, colour). Render-side gradient cache is stale; flag it.
    // No-op when the node doesn't have a gradient fill, so safe on
    // every appearance edit including stroke-only mutations.
    mark_gradient_cache_dirty(node);

    // Step 3: appearance-changed signal emission. Today the call sites
    // all directly trigger their own redraw paths (Canvas::queue_draw
    // via signal_invalidate on the active doc); routing redraws through
    // a single style-system signal is an optimisation pass, not a
    // correctness requirement.
}

bool materialise_from_style(SceneNode& node,
                            const StyleLibrary& lib) {
    // Lookup. Empty bound_style is a caller bug — shouldn't be calling
    // materialise on an unbound node — but the find_style will return
    // nullptr on empty anyway, so we fall through to the not-found branch
    // with a warning that's informative either way.
    const Style* s = lib.find_style(node.bound_style);
    if (!s) {
        LOG_WARN("style::materialise_from_style: id '{}' not found in library",
                 node.bound_style);
        return false;
    }

    // ── Fill projection ────────────────────────────────────────────────
    // Paint → FillStyle via the library-less overload. SwatchRef degrades
    // to its cached fallback colour; the id is carried onto fill_swatch_id
    // below so live_recolour_walk still finds the binding if the
    // underlying swatch is later edited.
    node.fill            = color::to_fillstyle(s->fill);
    node.fill_swatch_id  = swatch_id_of(s->fill);

    // ── Stroke projection ──────────────────────────────────────────────
    // Field-by-field map. Paint lands via the same bridge; cap/join are
    // the same enums on both sides; width and miter_limit are scalars.
    node.stroke.paint       = color::to_fillstyle(s->stroke.paint);
    node.stroke_swatch_id   = swatch_id_of(s->stroke.paint);
    node.stroke.width       = s->stroke.width;
    node.stroke.cap         = s->stroke.cap;
    node.stroke.join        = s->stroke.join;
    node.stroke.miter       = s->stroke.miter_limit;
    // stroke.opacity is deliberately NOT touched — StrokeAppearance has
    // no opacity concept, so the SceneNode's existing value survives.

    // ── Shadow projection (S98) ────────────────────────────────────────
    // 1:1 field map onto SceneNode's loose shadow_* fields. Always runs
    // — including when shadow.enabled is false — so binding to a "no
    // shadow" style turns shadow off on the host. The Style is the
    // appearance, including the absence of a shadow.
    node.shadow_enabled = s->shadow.enabled;
    node.shadow_dx      = s->shadow.dx;
    node.shadow_dy      = s->shadow.dy;
    node.shadow_blur    = s->shadow.blur;
    node.shadow_color_r = s->shadow.color_r;
    node.shadow_color_g = s->shadow.color_g;
    node.shadow_color_b = s->shadow.color_b;
    node.shadow_color_a = s->shadow.color_a;
    node.shadow_opacity = s->shadow.opacity;

    // S106 m1 — fill projected from the style; gradient cache is stale.
    mark_gradient_cache_dirty(node);

    LOG_TRACE("style::materialise_from_style: applied '{}' to node iid='{}'",
              node.bound_style, node.internal_id);
    return true;
}

// Shared scene-walker for style-binding propagation. Visits every node
// in the subtree under `node` and invokes `fn(*n)` whenever
// n->bound_style is non-empty. The structural shape (which slots to
// descend into) was duplicated across CurvzProject::load's post-load
// walk and Canvas's cross-doc propagation walk before m3d; it lives
// here now so both sites share the definition.
//
// Slot list mirrors live_recolour_walk in Canvas.cpp:
//   children, clip_shape, blend_source_a, blend_source_b, blend_cache,
//   warp_source, warp_glyph_cache, warp_cache.
//
// New stylable slots added to SceneNode in future phases need to be
// reflected here AND in live_recolour_walk; both serve the same role
// (cache propagation across the bound-binding graph) and getting one
// without the other would silently break propagation for the new slot.
void walk_style_bindings(SceneNode* node,
                         const std::function<void(SceneNode&)>& fn) {
    if (!node) return;

    if (!node->bound_style.empty() && fn) {
        fn(*node);
    }

    for (auto& child : node->children)
        walk_style_bindings(child.get(), fn);
    if (node->clip_shape)
        walk_style_bindings(node->clip_shape.get(), fn);
    if (node->blend_source_a)
        walk_style_bindings(node->blend_source_a.get(), fn);
    if (node->blend_source_b)
        walk_style_bindings(node->blend_source_b.get(), fn);
    for (auto& step : node->blend_cache)
        walk_style_bindings(step.get(), fn);
    if (node->warp_source)
        walk_style_bindings(node->warp_source.get(), fn);
    if (node->warp_glyph_cache)
        walk_style_bindings(node->warp_glyph_cache.get(), fn);
    if (node->warp_cache)
        walk_style_bindings(node->warp_cache.get(), fn);
}

} // namespace style
} // namespace Curvz
