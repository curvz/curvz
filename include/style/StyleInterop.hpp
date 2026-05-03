#pragma once
//
// StyleInterop.hpp — the funnel between Style edits and SceneNode appearance.
//
// Per ARCHITECTURE.md "Style System → Mutate-appearance funnel":
//
//   Critical for break-on-override correctness. Every site that writes to
//   node.fill, node.stroke, or node.shadow_* (inspector, toolbar,
//   eyedropper, paste, defaults application, paint-bucket, swatch-apply,
//   anywhere) goes through mutate_appearance(). The funnel:
//
//     1. If node.bound_style is non-empty, clear it (override = unbind).
//     2. Run the caller's mutator on the node — caller writes fill /
//        stroke / shadow however they like.
//
//   Style-driven updates (signal_style_changed propagation, bind_style)
//   are the *only* writers that bypass the funnel — they write fill /
//   stroke / shadow directly because they're the source of truth, not
//   an override. Callers in the propagation path use
//   materialise_from_style() instead.
//
//   S98: shadow joined the funnel scope when ShadowAppearance landed on
//   Style. Pre-S98 the inspector wrote shadow_* fields directly;
//   PropertiesPanel was migrated in lockstep with this header change.
//
// What is NOT a migration target (writes fill/stroke directly):
//
//   * Structural copy — clone_node, compound wrap/unwrap, boolean-op result
//     styling, blend-step interpolation, warp glyph cache. These are
//     internal structural operations, not user overrides. They preserve
//     whatever binding the source carried (clone_node explicitly copies
//     bound_style; boolean/blend/compound result-styling paths inherit
//     from an operand that already carries its own binding).
//   * Undo/redo restore — EditAppearanceCommand and friends own the full
//     before/after state including bound_style; they write fill/stroke
//     directly to restore the snapshot.
//   * SVG parser on load — parser sets bound_style and fill/stroke from
//     the file; no user override is happening.
//   * Style propagation (materialise_from_style) — the bound case, where
//     fill/stroke IS the cache, not an override.
//
// Future direction — Writer-style override tracking:
//
//   The v1 model is break-on-override: any direct edit clears bound_style,
//   the object freezes at its current appearance, and future edits to the
//   original style don't reach it. Simple, matches the addendum.
//
//   The longer-term direction (per Scott's steer, S74) is Writer-style:
//   bound_style stays the binding key, and a sibling StyleOverrides field
//   records which per-attribute values the user has overridden locally.
//   Editing the style propagates only to non-overridden attributes;
//   overridden ones keep their local values. An explicit "Clear direct
//   formatting" action zeroes the overrides and re-materialises.
//
//   When that flip happens:
//     1. Add `StyleOverrides style_overrides;` field to SceneNode.
//     2. Funnel body changes from `bound_style.clear()` to "for each
//        attribute the fn mutated, set the matching override flag."
//        Requires the funnel to know what was mutated — either via a
//        before/after diff on the node, or by switching from a generic
//        fn to a set of attribute-specific mutate_fill / mutate_stroke_*
//        funnel entry points (probably cleaner).
//     3. materialise_from_style skips overridden attributes.
//     4. SVG round-trip gains data-curvz-style-overrides; old projects
//        that lack the attribute decode as "no overrides" — additive
//        schema change, no migration needed.
//
//   None of that lands in Phase 1. The scaffolding here is compatible
//   with the future flip — nothing about the v1 funnel signature or
//   bound_style semantics precludes it.
//
// Why not template on the mutator type:
//   std::function<void(SceneNode&)> erases the lambda, costing one indirect
//   call. In return, the funnel definition lives in a .cpp (no header
//   bloat), and the SceneNode include stays out of every site that just
//   wants to declare a funnel call. Callers pay the cost of one virtual-
//   like dispatch per appearance write — irrelevant against the cost of
//   a redraw.
//

#include "style/Style.hpp"   // StyleId

#include <functional>

namespace Curvz {

// Forward-declare instead of including SceneNode.hpp — the funnel takes
// SceneNode by reference; only the .cpp needs the full definition. Same
// pattern as SwatchLibrary.hpp uses for set_paint.
struct SceneNode;

namespace style {

class StyleLibrary;

// The mutate-appearance funnel. Single chokepoint for every site that
// writes to node.fill or node.stroke outside of style-propagation. Clears
// node.bound_style first (override = unbind), then runs the caller's
// mutator.
//
//   fn: the actual mutation. Receives node by reference. Any combination
//       of node.fill / node.stroke / node.fill_swatch_id etc. is fair
//       game — the funnel doesn't inspect what changes, only that the
//       binding is broken first.
//
// Idempotent on already-unbound nodes: the bound_style check is cheap
// and skips the clear when already empty. Safe to call on defaults-
// application paths where the fresh node trivially has no binding.
void mutate_appearance(SceneNode& node,
                       const std::function<void(SceneNode&)>& fn);

// Project a Style's fill + StrokeAppearance onto a SceneNode's
// fill / stroke fields. Used by:
//
//   * The bind_style path — applying a style for the first time.
//   * The signal_style_changed propagation walk — re-syncing the cache
//     on every bound SceneNode after a style edit.
//   * Project load — re-materialising on every node whose bound_style
//     resolves to a style in the library.
//
// This is the *one* path that writes to node.fill / node.stroke without
// going through the funnel — because the cache update IS the source of
// truth in the bound case. Per addendum invariant 4.
//
// `lib` is consulted only for paint resolution context (SwatchRef
// fallback). For Phase 1 m2 the signature is in place but the body is
// a deliberate no-op stub — the actual Paint→FillStyle and
// StrokeAppearance→StrokeStyle projection lives in m3 alongside project
// save/load and SVG round-trip. Implementing the projection now would
// require a concrete binding UI to exercise it, which is Phase 2.
//
// Returns true if node.bound_style resolved in `lib` and the projection
// ran; false if the id wasn't found (in which case node.fill / node.stroke
// are untouched and the caller is expected to clear bound_style —
// that "dropped binding on missing style" path is a m3 concern).
bool materialise_from_style(SceneNode& node,
                            const StyleLibrary& lib);

// Recursive walk over a SceneNode subtree, invoking `fn` on every node
// whose bound_style is non-empty. The recursion descends into every
// structural slot that can hold stylable nodes — children, clip_shape,
// blend_source_a/b, blend_cache, warp_source, warp_glyph_cache,
// warp_cache. Mirrors the shape of live_recolour_walk in Canvas.cpp;
// kept here so the two existing call sites (project load
// re-materialisation and the cross-doc-propagation walk in Canvas)
// share one definition of "every binding-bearing slot in the scene".
//
// `fn` is invoked exactly once per bound SceneNode, by reference. The
// caller decides what action to take — id-filtering, materialise +
// drop on miss, materialise + log on miss, etc. The walk itself does
// not consult the library, does not filter by id, and does not mutate
// bound_style; those are all per-call-site policy.
//
// `fn` may be `{}` only as a no-op self-test; in that case the walk
// runs but performs no per-node work. Callers normally pass a bound
// lambda capturing the StyleLibrary and any id-filter they need.
//
// Walk order is deterministic but not topologically meaningful — it
// exists for completeness, not for any ordering guarantee. The
// recursion is depth-first, slots in the order listed above.
//
// Callers must not mutate the structural shape of the tree from
// inside `fn` (do not add/remove children, do not swap clip_shape,
// etc.). Mutating bound_style or fill/stroke fields on the visited
// node is fine — the walk holds no iterators that those writes
// could invalidate.
void walk_style_bindings(SceneNode* node,
                         const std::function<void(SceneNode&)>& fn);

} // namespace style
} // namespace Curvz
