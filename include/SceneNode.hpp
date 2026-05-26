#pragma once
#include <algorithm>
#include <cairomm/cairomm.h>  // S106 m1 — gradient_cache (Cairo::ImageSurface)
#include <cmath>
#include <glib.h> // g_uuid_string_random
#include <memory>
#include <string>
#include <vector>

namespace Curvz {

// ── Internal ID generator ────────────────────────────────────────────────────
// Generates a stable UUID for cross-document node linkage.
// Uses GLib's g_uuid_string_random() — RFC 4122 compliant, cryptographically
// random. Never reused. Separate from the SVG id attribute.
inline std::string generate_internal_id() {
  gchar *uuid = g_uuid_string_random();
  std::string result(uuid);
  g_free(uuid);
  return result;
}

// ── Style
// ─────────────────────────────────────────────────────────────────────
enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };

// ── Gradient stop ────────────────────────────────────────────────────────────
// One colour-stop on a gradient. offset is 0..1; r/g/b/a is the colour at that
// offset. Stops are kept sorted by offset on read; on write, sort defensively.
struct GradientStop {
  double offset = 0.0;
  double r = 0, g = 0, b = 0, a = 1;
};

// ── FillStyle ────────────────────────────────────────────────────────────────
// A paint specification. Solid fills use r/g/b/a directly. Gradient fills use
// the stops list plus geometry; the legacy r/g/b/a fields are ignored for
// gradient types but kept untouched so that flipping back to Solid restores
// the previous solid colour without surprise.
//
// Geometry is stored in **objectBoundingBox-fraction** space (0..1 of the
// shape's bbox). At render time the renderer lerps endpoints into doc-pixel
// space using the shape's actual bbox. This matches SVG's
// gradientUnits="objectBoundingBox" semantics, which is what S90 ships with;
// userSpaceOnUse comes later (will store endpoints in doc units instead).
//
// LinearGradient: g_x1,g_y1 → g_x2,g_y2 are the gradient line endpoints.
// RadialGradient: g_x1,g_y1 are the focal point (fx,fy); g_x2,g_y2 are the
// outer-circle centre (cx,cy); g_r is the outer-circle radius (also a 0..1
// fraction of the bbox's larger dimension).
struct FillStyle {
  enum class Type { None, CurrentColor, Solid, LinearGradient, RadialGradient };
  Type type = Type::CurrentColor;

  // Solid (and "remembered" for gradient types).
  double r = 0, g = 0, b = 0, a = 1;

  // Gradient stops (LinearGradient / RadialGradient). Empty for non-gradient.
  std::vector<GradientStop> stops;

  // Gradient geometry — meanings differ by Type, see struct doc above.
  double g_x1 = 0.0, g_y1 = 0.5;  // linear: start;  radial: fx,fy
  double g_x2 = 1.0, g_y2 = 0.5;  // linear: end;    radial: cx,cy
  double g_r  = 0.5;               // radial only:    outer radius

  // Convenience: is this fill a gradient of any kind?
  bool is_gradient() const {
    return type == Type::LinearGradient || type == Type::RadialGradient;
  }
};

struct StrokeStyle {
  FillStyle paint;
  double width = 0.0;
  LineCap cap = LineCap::Butt;
  LineJoin join = LineJoin::Miter;
  double miter = 4.0;
  double opacity = 1.0;
};

// ── Transform
// ─────────────────────────────────────────────────────────────────
struct Transform {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; // SVG matrix(a,b,c,d,e,f)
};

// ── ShadowParams ─────────────────────────────────────────────────────
// Snapshot bundle of all drop-shadow fields. Used by EditObjectCommand
// to capture pre/post-edit state in one value, mirroring how PathData /
// FillStyle / StrokeStyle are captured. NOT used for storage on
// SceneNode — the loose shadow_* fields stay primary so existing access
// patterns (obj.shadow_blur, obj.shadow_color_r) keep working everywhere.
//
// Defaults here intentionally match the SceneNode field defaults so a
// snapshot of an unmodified node matches a default-constructed
// ShadowParams, which keeps the "no diff" same-window undo branch tidy.
struct ShadowParams {
  bool   enabled = false;
  double dx      = 0.0;
  double dy      = 0.5;
  double blur    = 0.5;
  double color_r = 0.0;
  double color_g = 0.0;
  double color_b = 0.0;
  double color_a = 1.0;
  double opacity = 0.5;
};

// ── BezierNode
// ──────────────────────────────────────────────────────────────── One anchor
// point on a bezier path. Was: PathNode.
struct BezierNode {
  enum class Type { Smooth, Cusp, Symmetric, Corner };
  double x = 0, y = 0;     // anchor
  double cx1 = 0, cy1 = 0; // in-handle
  double cx2 = 0, cy2 = 0; // out-handle
  Type type = Type::Smooth;
};

// ── NodeSet
// ─────────────────────────────────────────────────────────────────── A set of
// BezierNodes linked by a shared constraint/kind. When any member moves, the
// constraint regenerates all others from params.
struct NodeSet {
  enum class Kind {
    Rect,    // params: cx,cy,w,h,rx,ry
    Ellipse, // params: cx,cy,rx,ry
    Star,    // params: cx,cy,r_outer,r_inner,points,angle_offset
    Polygon, // params: cx,cy,r,points,angle_offset
             // future: RoundedRect, Arc, Spiral, Gear, Arrow, ...
  };
  Kind kind;
  std::string name;
  std::vector<int> indices; // indices into PathData::nodes
  double params[8] = {};
};

// ── PathData
// ──────────────────────────────────────────────────────────────────
struct PathData {
  std::vector<BezierNode> nodes;
  std::vector<NodeSet> node_sets;
  bool closed = false;
};

// ── SceneNode
// ───────────────────────────────────────────────────────────────── Universal
// tree node. Type determines which fields are meaningful.
//
//   Layer  — top-level container. children = SceneNodes (Groups + Paths).
//             No geometry of its own. Has visible/locked/opacity.
//
//   Group  — geometric container. children = SceneNodes.
//             Has transform. fill/stroke ignored.
//
//   Path   — leaf. path != nullptr. children empty.
//             Has fill, stroke, opacity, transform.
//
//   ClipGroup — geometric container that clips children to the
//             geometry of `clip_shape` (a Path or Compound held in
//             its own slot, not in `children`). The clip_shape
//             renders only in outline mode. Deleting clip_shape
//             dissolves the ClipGroup (handled outside SceneNode).
//
//   Blend   — Illustrator-style shape interpolation container. Owns
//             two source Paths (blend_source_a, blend_source_b), each
//             in its own slot — NOT in `children`, which is unused on
//             Blend. Generates N intermediate Paths into blend_cache
//             on demand (cache is derived-not-authoritative: rebuilt
//             when blend_cache_dirty, never serialized as structural
//             data on SVG save, and never user-editable).
//             Node counts of A and B must match (equalized at Blend
//             construction; locked thereafter — any attempt to add/
//             remove a node on a source is rejected). Styles are
//             interpolated per step: fill colour, stroke colour,
//             stroke width (optionally overridden with a user range),
//             and opacity. Releasing a Blend dissolves it into A, B,
//             and a Group of the baked step paths as siblings.
//
//   Warp    — TypeStyler-style envelope distortion container. Takes a
//             single source (Path / Compound / Group, deep-cloned into
//             warp_source) and two boundary Bézier curves (warp_env_top,
//             warp_env_bottom) defining top and bottom of a ruled
//             surface. Points of the source are remapped through the
//             envelope: normalized (u,v) ∈ [0,1]² inside the source
//             bbox map to P(u,v) = (1-v)·C_bottom(u) + v·C_top(u).
//             Left and right sides are implicit straight lines between
//             the corner endpoints — trapezoidal/perspective warps
//             fall out naturally when top is shifted/shorter than
//             bottom. 2-anchor boundaries give arcs/balloons/
//             perspective; 3-4-anchor boundaries give wavy multi-cusp
//             distortions.
//
//             Two-stage cache so text sources can slot in later without
//             schema change:
//               warp_source      — untouched authoritative input
//               warp_glyph_cache — source-in-outline form (for path
//                                  sources, a clone; future text
//                                  sources render outlines here)
//               warp_cache       — final warped geometry, what the
//                                  canvas renders
//             Both caches are derived-not-authoritative: never
//             serialized as structural SVG, regenerated on dirty.
//
//             Like Blend, `children` is unused on Warp. Every tree
//             walker must descend consciously into warp_source /
//             warp_glyph_cache / warp_cache.
//
//             User lifecycle: Make Warp → dialog configures envelope
//             → Edit Warp re-opens dialog → Release Warp restores
//             source → Flatten Warp replaces Warp with baked paths.
//
struct SceneNode {
  enum class Type {
    Layer,
    Group,
    Path,
    Compound,
    GuideLayer,
    Guide,
    RefLayer,
    Ref,
    Text,
    Image,
    MeasureLayer,
    Measurement,
    GridLayer,
    MarginLayer,
    ClipGroup,
    Blend,
    Warp,
    // TextBox — typed container that owns a text frame as one user-
    // visible atom. Sibling to ClipGroup / Blend / Warp in the "typed
    // container" family: same kind of node, different rules about what
    // its children mean. The first-class type exists so "is this a
    // textbox?" is a compile-time-checkable type query, not a flag-
    // sniffing pattern over Group children. Construction, rendering,
    // and serialisation are added in later stages; introducing the
    // type by itself is a no-op at runtime (nothing constructs one
    // yet) and additive at compile time (every existing exhaustive
    // switch on Type has a default: clause; TextBox falls through).
    TextBox
  };

  Type type = Type::Path;
  std::string id; // SVG id attr — user-visible export name, not unique across docs
  // s168 m2: default-init via generate_internal_id() so every SceneNode
  // entering the tree has a non-empty iid by construction. Diagnosed in
  // s168 m1: Ref creation paths (Canvas_input.cpp:1690, :3876) and several
  // others mint nodes without calling generate_internal_id(), leaving
  // internal_id="". The s167 iid-based migration reads internal_id at
  // command-push time, and an empty iid silently no-ops on undo —
  // manifesting as RefMove "removing" the refpt. Default-init at the
  // struct level closes the gap structurally: clone_node and SvgParser
  // both overwrite internal_id explicitly after construction, so this
  // change is invisible to existing copy/load paths and only fills in
  // the gap for sites that previously skipped the assignment.
  std::string internal_id = generate_internal_id(); // Stable UUID — unique
                                                    // across all docs, used
                                                    // for cross-node links
  std::string name;
  bool visible = true;
  bool locked = false;
  double opacity = 1.0;
  std::string color; // layer color tag, hex e.g. "#e34c26", empty = none
  Transform transform;

  // Style — meaningful on Path and Text; ignored on Layer/Group/Compound/Guide
  FillStyle fill;
  StrokeStyle stroke;

  // Swatch binding — the paint IS a reference (see HANDOFF s69 "paint is a
  // reference, not a colour"). When *_swatch_id is non-empty, the paint on
  // this slot is a SwatchRef with that id, and the FillStyle above is a
  // render-path cache of the resolved colour (refreshed on every set_paint
  // and on live recolour when the referenced swatch is edited). When the
  // id is empty, the paint is whatever the FillStyle says — None,
  // CurrentColor, or Solid — and the FillStyle is authoritative.
  //
  // Empty id is the default for new nodes, matching the pre-S70 behaviour
  // where paints were flat FillStyles. Only the swatch-bound write path
  // (SwatchLibrary::set_paint with a SwatchRef) populates these. Clone and
  // SVG round-trip preserve them as a sibling of fill/stroke.
  //
  // Future gradient / pattern swatch kinds will keep this same field — the
  // renderer dispatches on the swatch variant, not on the FillStyle cache.
  std::string fill_swatch_id;
  std::string stroke_swatch_id;

  // Style binding — non-empty iff this node is bound to a Style in the
  // project's StyleLibrary. When bound, fill/stroke above are the runtime
  // cache of the style's current values — render path reads them as-is.
  // Direct user edits to fill/stroke must clear this field (override =
  // unbind); the mutate_appearance funnel in style/StyleInterop enforces
  // that invariant. Style-driven updates (signal_style_changed
  // propagation, explicit bind_style) write fill/stroke directly and
  // bypass the funnel — they are the source of truth, not an override.
  //
  // Per ARCHITECTURE.md "Style System". SVG round-trip (m3): emit as
  // data-curvz-style; resolve against current project library on load,
  // drop silently if the id doesn't resolve.
  std::string bound_style;

  // ── Gradient fill cache (S106 m1) ────────────────────────────────────
  // Render-side cache of the rasterised gradient fill for this node.
  // Cairo software-rasterises gradients on every frame; for any object
  // dragged across the canvas this means re-painting the same gradient
  // from scratch ~60 times per second. Caching the rasterised pixels
  // and blitting them — translate-and-blit instead of re-rasterise —
  // makes drag effectively free for gradient-filled objects.
  //
  // ── Coherence model (mixed: dirty flag + auto-detect) ─────────────
  // Two classes of invalidation, each with its own mechanism:
  //
  //   FILL CONTENT changes (gradient stop edits, fill type swaps,
  //   swatch live-recolour) — invalidated by an explicit dirty flag
  //   set at the mutate_appearance / materialise_from_style /
  //   live_recolour_walk funnels. Three call sites total; greppable
  //   via mark_gradient_cache_dirty(.
  //
  //   GEOMETRY changes (path-node mutations, scale, rotate, transform)
  //   — auto-detected at the render seam by comparing the cached bbox
  //   against the current bbox. No flag, no instrumentation per
  //   command. The cache key IS its bbox; if the bbox has changed,
  //   the cache is stale by definition and the seam rebuilds. Move
  //   does NOT invalidate — translation preserves both bbox shape and
  //   gradient, only the blit's target position changes.
  //
  // Lifecycle:
  //   gradient_cache:        the rasterised fill, sized to the node's
  //                          fill bbox at gradient_cache_zoom resolution.
  //                          Empty (RefPtr null) until first paint of a
  //                          gradient-filled node.
  //   gradient_cache_dirty:  true when the fill CONTENT is stale (stops,
  //                          colours, gradient type). Cleared on rebuild.
  //                          Geometry staleness uses the bbox check
  //                          below, not this flag.
  //   gradient_cache_bbox_*: the bbox the cache was rasterised for.
  //                          Render seam compares the SHAPE (w, h) of
  //                          the cache's bbox against the current
  //                          bbox; mismatch → rebuild. Position (x, y)
  //                          is NOT part of the coherence key —
  //                          translating an object preserves the
  //                          cached pixels and only changes the blit's
  //                          target offset. (-1 width signals "never
  //                          built" and is the initial state.)
  //   gradient_cache_zoom:   the m_zoom level at which the cache was
  //                          rasterised. Used by the render path to
  //                          decide whether to bilinear-upscale (zoom
  //                          drifted up since the cache built) or to
  //                          mark dirty for a higher-resolution rebuild
  //                          on zoom-settle. 0.0 = never built.
  //
  // Stroke is NOT cached. Strokes rasterise live every frame so they
  // stay crisp at any zoom and don't blur when the cache zoom drifts.
  // Only fills go through this cache.
  //
  // Clone: the cache is render-derived and node-keyed, so clones get a
  // fresh dirty cache rather than sharing the source's surface. The
  // first paint of the clone re-rasterises at its (possibly different)
  // bbox / position. See clone_node below.
  // Fields are `mutable` so the render-side cache machinery can fill
  // them through a const SceneNode& reference. The cache is physical
  // state (rasterised pixels for the current logical state), not
  // logical state — node identity, fill, geometry are unchanged when
  // the cache fills in. Standard C++ idiom for lazy caches behind a
  // logically-const interface.
  mutable Cairo::RefPtr<Cairo::ImageSurface> gradient_cache;
  mutable bool gradient_cache_dirty = true;
  mutable double gradient_cache_zoom = 0.0;
  mutable double gradient_cache_bbox_x = 0.0;
  mutable double gradient_cache_bbox_y = 0.0;
  mutable double gradient_cache_bbox_w = -1.0;  // -1 width = never built
  mutable double gradient_cache_bbox_h = 0.0;

  // Children — meaningful on Layer, Group, Compound, GuideLayer, and ClipGroup
  std::vector<std::unique_ptr<SceneNode>> children;

  // Clip shape — meaningful on ClipGroup only.
  //   The Path or Compound whose geometry defines the clip region.
  //   Held in its own slot rather than in `children` so its role is
  //   unambiguous and it doesn't get z-order-reversed with the clipped
  //   objects on SVG emit/parse. clip_id is the generated SVG defs id
  //   (e.g. "cp_<uuid>"); stable across saves once created.
  std::unique_ptr<SceneNode> clip_shape;
  std::string clip_id;

  // Blend data — meaningful on Blend only.
  //   blend_source_a / blend_source_b: the two paths being interpolated.
  //     Owned by this Blend node (deep-cloned from the originals at
  //     construction). Not in `children` — `children` is unused on
  //     Blend. Every tree walker (LayersPanel, SvgWriter, SvgParser,
  //     clone_node, collect_paths, collect_text_image_leaves) must
  //     descend consciously into these slots.
  //   blend_steps: number of intermediate paths generated between A
  //     and B (not counting A/B themselves). Range: 1..50.
  //   blend_cache: the generated intermediates, regenerated when
  //     blend_cache_dirty is true. Derived-not-authoritative: never
  //     persisted to SVG as structure, never user-editable.
  //   blend_cache_dirty: set to true whenever A, B, blend_steps, or
  //     any style parameter changes. Cleared after rebuild in draw.
  //   blend_stroke_w_user_set: when true, stroke widths of steps
  //     interpolate blend_stroke_w_start → blend_stroke_w_end (in doc
  //     units) regardless of A/B stroke widths. When false, they
  //     interpolate A.stroke.width → B.stroke.width.
  int blend_steps = 4;
  std::unique_ptr<SceneNode> blend_source_a;
  std::unique_ptr<SceneNode> blend_source_b;
  std::vector<std::unique_ptr<SceneNode>> blend_cache;
  bool   blend_cache_dirty       = true;
  bool   blend_stroke_w_user_set = false;
  double blend_stroke_w_start    = 0.0;
  double blend_stroke_w_end      = 0.0;

  // Warp data — meaningful on Warp only.
  //
  //   warp_source: the authoritative input, deep-cloned from the user's
  //     selection at Make Warp time. Usually a Path, Compound, or Group.
  //     Never mutated by the Warp pipeline. Released back into the tree
  //     on Release Warp.
  //
  //   warp_glyph_cache: source resolved to outline form. For Path/
  //     Compound/Group sources this is a deep clone of warp_source (the
  //     source IS already outlines). Exists as a distinct stage so that
  //     future Text sources can render glyph outlines into it without
  //     any other part of the pipeline noticing. Derived-not-
  //     authoritative: rebuilt when warp_glyph_cache_dirty is true,
  //     never serialized as SVG structure.
  //
  //   warp_env_top / warp_env_bottom: the two boundary curves of the
  //     envelope, each a PathData with 2, 3, or 4 anchors. Left and
  //     right sides of the envelope are implicit straight lines joining
  //     the top and bottom endpoints — no separate left/right storage.
  //     Top and bottom anchor counts are independent (they don't need
  //     to match). Both stored in doc space, Y-up. The Warp's own
  //     transform is applied on top of envelope coordinates at render.
  //
  //   warp_cache: the final warped geometry — what the canvas actually
  //     draws. Derived-not-authoritative: rebuilt by rebuild_warp_cache
  //     when warp_cache_dirty is true, never persisted as SVG structure.
  //     Shape mirrors warp_glyph_cache (Path→Path, Group→Group of
  //     warped children, Compound→Compound of warped children) so that
  //     downstream walkers see a familiar tree.
  //
  //   warp_glyph_cache_dirty: true when warp_source changed since last
  //     glyph-cache rebuild. Changing the source invalidates both caches
  //     (glyph first, then warp). Changing only the envelope invalidates
  //     only warp_cache_dirty.
  //
  //   warp_cache_dirty: true when warp_glyph_cache or either envelope
  //     curve changed since last warp-cache rebuild. Cleared by
  //     rebuild_warp_cache.
  //
  //   `children` is unused on Warp — all payload lives in the slots
  //   above. Every tree walker (LayersPanel, SvgWriter, SvgParser,
  //   clone_node, collect_paths, collect_text_image_leaves,
  //   PropertiesPanel::collect_leaves, mark_all_warps_dirty,
  //   find_warp_owner, object_bbox, hit_test) must descend into
  //   warp_source / warp_glyph_cache / warp_cache consciously.
  std::unique_ptr<SceneNode> warp_source;
  std::unique_ptr<SceneNode> warp_glyph_cache;
  bool                       warp_glyph_cache_dirty = true;
  PathData                   warp_env_top;
  PathData                   warp_env_bottom;
  std::unique_ptr<SceneNode> warp_cache;
  bool                       warp_cache_dirty = true;
  // warp_quality: subdivision density for rebuild_warp_cache. Each source
  //   cubic segment in warp_glyph_cache is split into warp_quality equal
  //   t-slices in warp_cache. Higher = smoother curves but larger cache
  //   and slower rebuild. Range [1, 16]. 1 = no subdivision (one cubic
  //   in, one cubic out; visibly faceted under strong envelopes).
  //   4 = default balance of smoothness and speed. 16 = print quality.
  //   Changing this invalidates warp_cache_dirty only (glyph cache is
  //   unaffected). Serialized as data-curvz-warp-quality="N" (M6).
  int                        warp_quality = 4;

  // ── Warp preset provenance (s147 m2) ────────────────────────────────
  // Three fields capture "what preset was last applied to this warp,
  // and what counts." Persisted across save/load; survive clone; reset
  // on envelope drift (handle drag, arrow-key nudge — the inspector's
  // dropdown then shows (Custom) honestly).
  //
  // warp_preset_idx: -1 = Custom (envelope is hand-edited or no preset
  //   has ever been applied). 0..warp_presets::PRESET_COUNT-1 = a
  //   real preset value (FLAT, ARC_UP, ...). Inspector dropdown
  //   reads this directly and biases its initial selection.
  //
  // warp_top_count / warp_bot_count: anchor counts last fed to the
  //   preset generator. Range [2, 4]. Used by inspector spinners on
  //   refresh so a saved Bulge with top=4, bot=2 reloads with those
  //   numbers showing — not envelope-size-derived (which would also
  //   work for preset-conformant envelopes but loses provenance for
  //   the "saved Custom-edited shape" case).
  //
  // Clearing rule: when the envelope is mutated by anything other than
  //   a preset application (handle drag commit, arrow nudge, future
  //   inspector hand-edits), the editing command sets warp_preset_idx
  //   = -1 in the post-state so undo restores both envelope shape AND
  //   preset label atomically. Translation of the whole warp object
  //   does NOT count as drift — source moves with envelope, the
  //   preset relationship is preserved.
  //
  // Serialized as data-curvz-warp-preset / data-curvz-warp-top-count /
  // data-curvz-warp-bot-count. preset attr omitted when -1 (Custom is
  // the implicit default).
  int                        warp_preset_idx = -1;
  int                        warp_top_count  = 2;
  int                        warp_bot_count  = 2;

  // ── Drop shadow ──────────────────────────────────────────────────────
  // S97 m1. A presentational effect on the host node, in the same family
  // as fill / stroke / opacity. Meaningful on Path, Text, Group, Compound,
  // ClipGroup, Blend, Warp — anywhere a render pass exists. v1 is a single
  // shadow per object (Affinity-style stacks are a future promotion: this
  // field block would migrate into effects[0] of an effects vector).
  //
  //   shadow_enabled  — gate. False = no shadow at all (no SVG <filter>
  //                     emitted, no Cairo render pass, fields ignored).
  //                     False is the default for new nodes; explicit
  //                     opt-in keeps existing files visually unchanged.
  //   shadow_dx, _dy  — offset of the shadow from the host, in doc units.
  //                     Doc Y-down: positive dy is "below", negative is
  //                     "above". Matches SVG <feOffset>.
  //   shadow_blur     — Gaussian blur radius in doc units (one stddev).
  //                     0.0 = sharp offset, no blur. Cairo m2 will
  //                     approximate via box-blur passes; SVG emits as
  //                     stdDeviation on <feGaussianBlur>.
  //   shadow_color_*  — shadow tint, premultiplied with shadow_opacity at
  //                     render. Defaults to opaque black. Independent of
  //                     fill/stroke colour bindings — shadows are not
  //                     swatch-bound in v1 (could become so via the
  //                     paint-is-a-reference pattern in a later session).
  //   shadow_opacity  — final alpha multiplier on the shadow, after the
  //                     colour's own alpha. Range 0..1. Lets the user
  //                     dial down a saturated shadow without re-picking.
  //
  // SVG round-trip: dual-channel.
  //   - Native <filter> emitted in <defs> as id "sh_<short_iid>", host
  //     node references it via filter="url(#sh_<short_iid>)". Renders
  //     correctly in browsers / Inkscape / Folio. Write-only — Curvz
  //     never parses foreign <filter> elements.
  //   - Mirror of all fields as data-curvz-shadow-* attributes on the
  //     host's open tag. Source of truth on Curvz load. Same dual-source
  //     pattern as name (data-curvz-name) over SVG id.
  // Both emitted only when shadow_enabled. Absent both = clean tag.
  //
  // Defaults (S97 m4 tune): sub-unit values for icon-canvas use. Curvz's
  // primary deliverable is 16/24/32/48-px Folio icons; on a 24-px canvas
  // a 4-px offset is 1/6 of the height, far too dramatic. Smaller defaults
  // keep first-toggle visually sane on the common case; larger documents
  // can dial up.
  bool   shadow_enabled = false;
  double shadow_dx      = 0.0;
  double shadow_dy      = 0.5;
  double shadow_blur    = 0.5;
  double shadow_color_r = 0.0;
  double shadow_color_g = 0.0;
  double shadow_color_b = 0.0;
  double shadow_color_a = 1.0;
  double shadow_opacity = 0.5;

  // Path data — meaningful on Path only
  std::unique_ptr<PathData> path;

  // Guide data — meaningful on Guide only.
  //
  // A guide is a directed line through (guide_x, guide_y) at angle
  // guide_angle (degrees, 0=horizontal, 90=vertical, positive rotates
  // CW in doc-Y-down space — matches Canvas::rotate_selection_by).
  // The anchor (x,y) is the "home" point on the line:
  //   - ruler-dragged H/V:     canvas-center-on-line
  //   - two-point constructed: midpoint of the two clicks
  //
  // All values in doc space (Y-down, raw canvas pixels).
  double guide_x     = 0.0;
  double guide_y     = 0.0;
  double guide_angle = 0.0;

  // Ref point data — meaningful on Ref only
  double ref_x = 0.0;
  double ref_y = 0.0;

  // Text data — meaningful on Text only
  std::string text_content;
  double text_x = 0.0; // anchor x in doc space (Y-up)
  double text_y = 0.0; // anchor y in doc space (Y-up)
  std::string text_font_family = "Sans";
  double text_font_size = 24.0; // in document units
  bool text_bold = false;
  bool text_italic = false;
  std::string text_anchor = "start"; // SVG text-anchor: "start"|"middle"|"end"
  std::string text_align =
      "left"; // paragraph align: "left"|"center"|"right"|"justify"
  double text_baseline_shift =
      0.0; // perpendicular offset from path/baseline in doc units
  double text_letter_spacing = 0.0; // extra advance between glyphs in doc units
  // Text-on-path fields — meaningful on Text only when text_path_id is
  // non-empty
  std::string text_path_id; // id of guide path SceneNode; empty = normal text
  double text_path_offset =
      0.0; // arc-length start offset along path in doc units
  bool text_path_flip = false; // true = text hangs below path (inside circle)

  // ── s301 m1a — Text container model (data foundation) ──────────────────────
  // The unified text model (see docs/text_unified_model.md) decomposes text
  // rendering into three primitives: a boundary (closed path defining where
  // text appears), a line pattern (path each line follows, default horizontal),
  // and the text content itself. A text node binds to zero or more boundary
  // paths and optionally one line-pattern path by iid.
  //
  // Empty list / empty id = legacy unbound text — renders using text_x/text_y
  // and the existing draw_text_node code path, fully backward compatible. As
  // milestone 1b wires the renderer to honor these bindings, an unbound text
  // will keep behaving as it does today and a bound text will route through
  // the new container-aware path.
  //
  // Threading: when text overflows the first boundary, it continues in the
  // second, and so on. A boundary participates in at most one chain — see
  // text_unified_model.md "Valid candidates" rule.
  std::vector<std::string> text_boundary_ids;
  // Optional line-pattern path. Empty = default horizontal line pattern.
  // When set, every line of text follows this path, duplicated downward at
  // the leading distance. This is how text-on-path is expressed in the
  // unified model — set the line pattern, the renderer applies the rules.
  std::string text_line_path_id;
  // Inset margins from the boundary edge inward (doc units). Text flows in
  // the inset region, not the raw boundary. Zero on all four sides = boundary
  // edge IS the text edge.
  //
  // Ownership rule (post-TextBox-migration):
  //
  //   For TextBox-owned text, margins live on the BOUNDARY child
  //   (children[1]). The boundary defines the shape; the margin is an
  //   inset operation on the shape. The text child does not carry
  //   margins. Readers go through `effective_text_margins` below, which
  //   makes the rule structural rather than something each call site
  //   has to remember.
  //
  //   For legacy paired-sibling text (loaded from before the migration),
  //   margins live on the TEXT node. The boundary has zero. The same
  //   helper reads from whichever side is non-zero, so legacy files
  //   continue to lay out correctly without changing the readers.
  //
  //   The four values are independent — top/bottom/left/right can each
  //   take any non-negative value. Current UI sets them uniformly; a
  //   future UI exposes them per side without any structural change.
  double text_margin_top    = 0.0;
  double text_margin_bottom = 0.0;
  double text_margin_left   = 0.0;
  double text_margin_right  = 0.0;

  // Image data — meaningful on Image only
  std::string image_path; // absolute path to the image file
  double image_x = 0.0;   // top-left x in doc space (Y-down)
  double image_y = 0.0;   // top-left y in doc space (Y-down)
  double image_w = 0.0;   // width in doc units
  double image_h = 0.0;   // height in doc units

  // Measurement data — meaningful on Measurement only
  // All coords in doc space, Y-up (user space)
  double measure_x1 = 0.0;
  double measure_y1 = 0.0;
  double measure_x2 = 0.0;
  double measure_y2 = 0.0;

  // Grid data — meaningful on GridLayer only
  double grid_spacing_x = 100.0; // doc units between vertical lines
  double grid_spacing_y = 100.0; // doc units between horizontal lines
  double grid_offset_x  = 0.0;   // doc units offset from canvas origin X
  double grid_offset_y  = 0.0;   // doc units offset from canvas origin Y
  double grid_color_r   = 0.5;
  double grid_color_g   = 0.5;
  double grid_color_b   = 0.8;
  double grid_color_a   = 0.35;
  bool   grid_dots      = false;  // false=lines, true=dots at intersections
  bool   grid_snap      = false;

  // Margin / column data — meaningful on MarginLayer only.
  // Margins define insets from canvas edges.
  // Columns/rows subdivide the area inside the margins, with an explicit
  // gap (gutter) between each column and row — same model as InDesign.
  double margin_top      = 0.0;  // doc units inset from top edge
  double margin_bottom   = 0.0;
  double margin_left     = 0.0;
  double margin_right    = 0.0;
  int    margin_columns  = 1;
  double margin_col_gap  = 0.0;  // gap between columns (InDesign: "Gutter")
  int    margin_rows     = 1;
  double margin_row_gap  = 0.0;  // gap between rows
  double margin_color_r  = 0.8;
  double margin_color_g  = 0.3;
  double margin_color_b  = 0.3;
  double margin_color_a  = 0.15;

  // ── Convenience ───────────────────────────────────────────────────────
  bool is_layer()        const { return type == Type::Layer; }
  bool is_group()        const { return type == Type::Group; }
  bool is_path()         const { return type == Type::Path; }
  bool is_compound()     const { return type == Type::Compound; }
  bool is_clip_group()   const { return type == Type::ClipGroup; }
  bool is_blend()        const { return type == Type::Blend; }
  bool is_warp()         const { return type == Type::Warp; }
  bool is_guide_layer()  const { return type == Type::GuideLayer; }
  bool is_guide()        const { return type == Type::Guide; }
  bool is_ref_layer()    const { return type == Type::RefLayer; }
  bool is_ref()          const { return type == Type::Ref; }
  bool is_text()         const { return type == Type::Text; }
  bool is_text_box()     const { return type == Type::TextBox; }
  bool is_image()        const { return type == Type::Image; }
  bool is_measure_layer()  const { return type == Type::MeasureLayer; }
  bool is_measurement()    const { return type == Type::Measurement; }
  bool is_grid_layer()     const { return type == Type::GridLayer; }
  bool is_margin_layer()   const { return type == Type::MarginLayer; }

  // ── Shadow helpers (S97 m3) ─────────────────────────────────────────
  // One-line snapshot/restore for inspector undo and any other site that
  // wants to bundle the nine shadow_* fields. The SceneNode keeps the
  // loose fields as primary storage; ShadowParams is purely a transport.
  ShadowParams read_shadow() const {
    ShadowParams s;
    s.enabled = shadow_enabled;
    s.dx      = shadow_dx;
    s.dy      = shadow_dy;
    s.blur    = shadow_blur;
    s.color_r = shadow_color_r;
    s.color_g = shadow_color_g;
    s.color_b = shadow_color_b;
    s.color_a = shadow_color_a;
    s.opacity = shadow_opacity;
    return s;
  }
  void write_shadow(const ShadowParams& s) {
    shadow_enabled = s.enabled;
    shadow_dx      = s.dx;
    shadow_dy      = s.dy;
    shadow_blur    = s.blur;
    shadow_color_r = s.color_r;
    shadow_color_g = s.color_g;
    shadow_color_b = s.color_b;
    shadow_color_a = s.color_a;
    shadow_opacity = s.opacity;
  }

  // True for any node type that supports the drop-shadow effect — i.e.
  // anything with a render pass. Excludes structural-only types (Layer,
  // GuideLayer, Guide, RefLayer, Ref, MeasureLayer, Measurement,
  // GridLayer, MarginLayer). The inspector uses this to decide whether
  // to show the shadow section.
  bool can_have_shadow() const {
    switch (type) {
      case Type::Path:
      case Type::Compound:
      case Type::Group:
      case Type::Text:
      case Type::Image:
      case Type::ClipGroup:
      case Type::Blend:
      case Type::Warp:
      case Type::TextBox:
        return true;
      default:
        return false;
    }
  }

  // ── Guide helpers ────────────────────────────────────────────────────
  // Axis-aligned guide checks (used by M1-era snap/hit-test which only
  // handle H and V).  Arbitrary-angle guides return false for both.
  // Angle is normalized into [0, 180) for the check.
  bool guide_is_horizontal() const {
    if (type != Type::Guide) return false;
    double a = guide_angle;
    a = a - std::floor(a / 180.0) * 180.0;
    return std::abs(a) < 1e-6 || std::abs(a - 180.0) < 1e-6;
  }
  bool guide_is_vertical() const {
    if (type != Type::Guide) return false;
    double a = guide_angle;
    a = a - std::floor(a / 180.0) * 180.0;
    return std::abs(a - 90.0) < 1e-6;
  }
  // True if guide is axis-aligned (H or V).
  bool guide_is_axis_aligned() const {
    return guide_is_horizontal() || guide_is_vertical();
  }
  // True for any non-artwork system layer (guide, ref, measure, grid, margin)
  bool is_special_layer()  const {
    return is_guide_layer() || is_ref_layer() || is_measure_layer() ||
           is_grid_layer() || is_margin_layer();
  }
};

// ── ScenePath
// ───────────────────────────────────────────────────────────────── Address of
// any node in the tree as a sequence of child indices from root. e.g. {0, 2, 1}
// = layers[0]->children[2]->children[1]
using ScenePath = std::vector<int>;

// ── Deep clone
// ────────────────────────────────────────────────────────────────
inline std::unique_ptr<SceneNode> clone_node(const SceneNode &src) {
  auto dst = std::make_unique<SceneNode>();
  dst->type = src.type;
  dst->id = src.id;
  dst->internal_id = src.internal_id;
  dst->name = src.name;
  dst->visible = src.visible;
  dst->locked = src.locked;
  dst->opacity = src.opacity;
  dst->color = src.color;
  dst->transform = src.transform;
  dst->fill = src.fill;
  dst->stroke = src.stroke;
  dst->fill_swatch_id = src.fill_swatch_id;
  dst->stroke_swatch_id = src.stroke_swatch_id;
  dst->bound_style = src.bound_style;
  if (src.path)
    dst->path = std::make_unique<PathData>(*src.path);
  dst->guide_x = src.guide_x;
  dst->guide_y = src.guide_y;
  dst->guide_angle = src.guide_angle;
  dst->ref_x = src.ref_x;
  dst->ref_y = src.ref_y;
  dst->text_content = src.text_content;
  dst->text_x = src.text_x;
  dst->text_y = src.text_y;
  dst->text_font_family = src.text_font_family;
  dst->text_font_size = src.text_font_size;
  dst->text_bold = src.text_bold;
  dst->text_italic = src.text_italic;
  dst->text_anchor = src.text_anchor;
  dst->text_align = src.text_align;
  dst->text_baseline_shift = src.text_baseline_shift;
  dst->text_letter_spacing = src.text_letter_spacing;
  dst->text_path_id = src.text_path_id;
  dst->text_path_offset = src.text_path_offset;
  dst->text_path_flip = src.text_path_flip;
  // s301 m1a — text container model field copy
  dst->text_boundary_ids = src.text_boundary_ids;
  dst->text_line_path_id = src.text_line_path_id;
  dst->text_margin_top    = src.text_margin_top;
  dst->text_margin_bottom = src.text_margin_bottom;
  dst->text_margin_left   = src.text_margin_left;
  dst->text_margin_right  = src.text_margin_right;
  dst->image_path = src.image_path;
  dst->image_x = src.image_x;
  dst->image_y = src.image_y;
  dst->image_w = src.image_w;
  dst->image_h = src.image_h;
  dst->measure_x1 = src.measure_x1;
  dst->measure_y1 = src.measure_y1;
  dst->measure_x2 = src.measure_x2;
  dst->measure_y2 = src.measure_y2;
  // ClipGroup fields — clip_shape is deep-cloned, clip_id is copied as-is
  // so the clone continues to reference the same SVG defs id on export.
  // For duplicated ClipGroups a fresh clip_id should be minted by the
  // caller (matches how internal_id is regenerated for paste — see Canvas).
  dst->clip_id = src.clip_id;
  if (src.clip_shape)
    dst->clip_shape = clone_node(*src.clip_shape);
  // Blend fields — A/B slots deep-cloned; cache deep-cloned too so clones
  // render immediately (caller can force a rebuild by setting dirty if
  // sources have diverged since last build). blend_steps and stroke-width
  // overrides copy as-is.
  dst->blend_steps              = src.blend_steps;
  dst->blend_cache_dirty        = src.blend_cache_dirty;
  dst->blend_stroke_w_user_set  = src.blend_stroke_w_user_set;
  dst->blend_stroke_w_start     = src.blend_stroke_w_start;
  dst->blend_stroke_w_end       = src.blend_stroke_w_end;
  if (src.blend_source_a)
    dst->blend_source_a = clone_node(*src.blend_source_a);
  if (src.blend_source_b)
    dst->blend_source_b = clone_node(*src.blend_source_b);
  for (const auto &step : src.blend_cache)
    dst->blend_cache.push_back(clone_node(*step));
  // Warp fields — source, glyph cache, and warp cache are all deep-cloned
  // so the clone renders immediately at the same visual state as the
  // original. Envelope PathData copies by value (POD-ish struct). Dirty
  // flags copy as-is; caller can force a rebuild by flipping them.
  dst->warp_glyph_cache_dirty = src.warp_glyph_cache_dirty;
  dst->warp_cache_dirty       = src.warp_cache_dirty;
  dst->warp_env_top           = src.warp_env_top;
  dst->warp_env_bottom        = src.warp_env_bottom;
  dst->warp_quality           = src.warp_quality;
  // s147 m2: preset provenance fields — survive clone so undo
  // snapshots and copy/paste preserve the "this came from preset X"
  // metadata.
  dst->warp_preset_idx        = src.warp_preset_idx;
  dst->warp_top_count         = src.warp_top_count;
  dst->warp_bot_count         = src.warp_bot_count;
  if (src.warp_source)
    dst->warp_source = clone_node(*src.warp_source);
  if (src.warp_glyph_cache)
    dst->warp_glyph_cache = clone_node(*src.warp_glyph_cache);
  if (src.warp_cache)
    dst->warp_cache = clone_node(*src.warp_cache);
  // Drop shadow (S97 m1) — flat field block, copy by value.
  dst->shadow_enabled = src.shadow_enabled;
  dst->shadow_dx      = src.shadow_dx;
  dst->shadow_dy      = src.shadow_dy;
  dst->shadow_blur    = src.shadow_blur;
  dst->shadow_color_r = src.shadow_color_r;
  dst->shadow_color_g = src.shadow_color_g;
  dst->shadow_color_b = src.shadow_color_b;
  dst->shadow_color_a = src.shadow_color_a;
  dst->shadow_opacity = src.shadow_opacity;
  // Gradient cache (S106 m1) — render-derived, not copied. The clone
  // gets fresh defaults (gradient_cache null, gradient_cache_dirty true,
  // gradient_cache_zoom 0.0) from SceneNode's field initialisers, so
  // the first paint of the clone re-rasterises the gradient at the
  // clone's bbox/zoom. Sharing the source's surface across two nodes
  // that may invalidate independently would be incorrect.
  // Grid layer fields — meaningful only on GridLayer nodes, but copied
  // unconditionally (they're flat doubles/bools; no harm on other types).
  // These don't round-trip through SVG (project.json carries them), so
  // any code path that clones a grid layer must use this clone_node.
  dst->grid_spacing_x = src.grid_spacing_x;
  dst->grid_spacing_y = src.grid_spacing_y;
  dst->grid_offset_x  = src.grid_offset_x;
  dst->grid_offset_y  = src.grid_offset_y;
  dst->grid_color_r   = src.grid_color_r;
  dst->grid_color_g   = src.grid_color_g;
  dst->grid_color_b   = src.grid_color_b;
  dst->grid_color_a   = src.grid_color_a;
  dst->grid_dots      = src.grid_dots;
  dst->grid_snap      = src.grid_snap;
  // Margin layer fields — same shape as grid: meaningful only on
  // MarginLayer nodes, copied unconditionally, project.json-only.
  dst->margin_top     = src.margin_top;
  dst->margin_bottom  = src.margin_bottom;
  dst->margin_left    = src.margin_left;
  dst->margin_right   = src.margin_right;
  dst->margin_columns = src.margin_columns;
  dst->margin_col_gap = src.margin_col_gap;
  dst->margin_rows    = src.margin_rows;
  dst->margin_row_gap = src.margin_row_gap;
  dst->margin_color_r = src.margin_color_r;
  dst->margin_color_g = src.margin_color_g;
  dst->margin_color_b = src.margin_color_b;
  dst->margin_color_a = src.margin_color_a;
  for (const auto &child : src.children)
    dst->children.push_back(clone_node(*child));
  return dst;
}

// ── Gradient cache invalidation (S106 m1) ────────────────────────────────────
//
// Single funnel for fill-CONTENT invalidation. Call from any code path
// that mutates the gradient's stops, type, or colours:
//
//   - mutate_appearance (StyleInterop) — the funnel for all user-driven
//     appearance edits. Catches PaintEditor commits, gradient stop
//     edits, fill type swaps.
//   - materialise_from_style (StyleInterop) — bound-style propagation.
//     When a Style's gradient changes, every bound node gets its fill
//     re-projected through this path.
//   - live_recolour_walk (Canvas) — swatch-bound nodes get FillStyle
//     re-resolved when the swatch's colour changes.
//
// GEOMETRY changes (path-node mutations, scale, rotate) are NOT funnelled
// through here. The render seam (apply_fill in Canvas) compares the
// cached gradient_cache_bbox_* against the current bbox and rebuilds on
// mismatch. No per-command instrumentation needed.
//
// Designed as a free function rather than a member so it stays cheap to
// call from any TU without further coupling. Cost is one branch + one
// bool write — safe to spam.
//
// No-op when the node doesn't currently have a gradient fill: the cache
// only exists for LinearGradient / RadialGradient. Setting dirty on
// solid-filled nodes would be harmless but pointless.
inline void mark_gradient_cache_dirty(SceneNode& n) {
    if (n.fill.type == FillStyle::Type::LinearGradient ||
        n.fill.type == FillStyle::Type::RadialGradient) {
        n.gradient_cache_dirty = true;
    }
}

// ── Primitive → PathData converters ──────────────────────────────────────────
// Rect (with optional rounded corners). Returns PathData + NodeSet.
inline PathData rect_to_path(double x, double y, double w, double h,
                             double rx = 0.0, double ry = 0.0) {
  rx = std::min(rx, w * 0.5);
  ry = std::min(ry, h * 0.5);
  PathData pd;
  pd.closed = true;

  if (rx <= 0.0 && ry <= 0.0) {
    auto mk = [](double px, double py) {
      BezierNode n;
      n.x = px;
      n.y = py;
      n.cx1 = px;
      n.cy1 = py;
      n.cx2 = px;
      n.cy2 = py;
      n.type = BezierNode::Type::Corner;
      return n;
    };
    pd.nodes.push_back(mk(x, y));
    pd.nodes.push_back(mk(x + w, y));
    pd.nodes.push_back(mk(x + w, y + h));
    pd.nodes.push_back(mk(x, y + h));

    NodeSet ns;
    ns.kind = NodeSet::Kind::Rect;
    ns.name = "rect";
    ns.indices = {0, 1, 2, 3};
    ns.params[0] = x + w * 0.5;
    ns.params[1] = y + h * 0.5;
    ns.params[2] = w;
    ns.params[3] = h;
    ns.params[4] = 0;
    ns.params[5] = 0;
    pd.node_sets.push_back(ns);
    return pd;
  }

  constexpr double K = 0.5522847498;
  double kx = rx * K, ky = ry * K;
  double x0 = x, x1 = x + rx, x2 = x + w - rx, x3 = x + w;
  double y0 = y, y1 = y + ry, y2 = y + h - ry, y3 = y + h;
  auto mk = [](double px, double py, double ix, double iy, double ox,
               double oy) {
    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = ix;
    n.cy1 = iy;
    n.cx2 = ox;
    n.cy2 = oy;
    n.type = BezierNode::Type::Smooth;
    return n;
  };
  pd.nodes.push_back(mk(x1, y0, x1 - kx, y0, x1 + kx, y0));
  pd.nodes.push_back(mk(x2, y0, x2 - kx, y0, x2 + kx, y0));
  pd.nodes.push_back(mk(x3, y1, x3, y1 - ky, x3, y1 + ky));
  pd.nodes.push_back(mk(x3, y2, x3, y2 - ky, x3, y2 + ky));
  pd.nodes.push_back(mk(x2, y3, x2 + kx, y3, x2 - kx, y3));
  pd.nodes.push_back(mk(x1, y3, x1 + kx, y3, x1 - kx, y3));
  pd.nodes.push_back(mk(x0, y2, x0, y2 + ky, x0, y2 - ky));
  pd.nodes.push_back(mk(x0, y1, x0, y1 + ky, x0, y1 - ky));

  NodeSet ns;
  ns.kind = NodeSet::Kind::Rect;
  ns.name = "rect";
  ns.indices = {0, 1, 2, 3, 4, 5, 6, 7};
  ns.params[0] = x + w * 0.5;
  ns.params[1] = y + h * 0.5;
  ns.params[2] = w;
  ns.params[3] = h;
  ns.params[4] = rx;
  ns.params[5] = ry;
  pd.node_sets.push_back(ns);
  return pd;
}

inline PathData ellipse_to_path(double cx, double cy, double rx, double ry) {
  constexpr double K = 0.5522847498;
  double kx = rx * K, ky = ry * K;
  PathData pd;
  pd.closed = true;
  auto mk = [](double px, double py, double ix, double iy, double ox,
               double oy) {
    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = ix;
    n.cy1 = iy;
    n.cx2 = ox;
    n.cy2 = oy;
    n.type = BezierNode::Type::Smooth;
    return n;
  };
  pd.nodes.push_back(mk(cx, cy - ry, cx - kx, cy - ry, cx + kx, cy - ry));
  pd.nodes.push_back(mk(cx + rx, cy, cx + rx, cy - ky, cx + rx, cy + ky));
  pd.nodes.push_back(mk(cx, cy + ry, cx + kx, cy + ry, cx - kx, cy + ry));
  pd.nodes.push_back(mk(cx - rx, cy, cx - rx, cy + ky, cx - rx, cy - ky));

  NodeSet ns;
  ns.kind = NodeSet::Kind::Ellipse;
  ns.name = "ellipse";
  ns.indices = {0, 1, 2, 3};
  ns.params[0] = cx;
  ns.params[1] = cy;
  ns.params[2] = rx;
  ns.params[3] = ry;
  pd.node_sets.push_back(ns);
  return pd;
}

// ── polygon_to_path
// ─────────────────────────────────────────────────────────── Generates a
// regular polygon or star as a closed PathData.
//
// Parameters:
//   cx, cy     — centre in doc space
//   radius     — outer vertex radius
//   sides      — number of sides / outer points (≥ 3)
//   inflection — inner radius ratio [0.0, 1.0].
//                1.0 = regular polygon (no inner vertices)
//                cos(π/sides) = perfect star (sides of star are straight)
//                < cos(π/sides) = sharper star
//   angle_rad  — rotation of first outer vertex from top (Y-down: -π/2 = point
//   up)
//
// The "perfect star" snap ratio for N sides is cos(π/N).
// At inflection == 1.0 only outer vertices are emitted (N nodes).
// At inflection < 1.0 inner vertices are interleaved (2N nodes).
inline PathData polygon_to_path(double cx, double cy, double radius, int sides,
                                double inflection,
                                double angle_rad = -M_PI * 0.5) {
  if (sides < 3)
    sides = 3;
  inflection = std::max(0.01, std::min(1.0, inflection));

  PathData pd;
  pd.closed = true;

  bool is_star = (inflection < 0.9999);
  int total = is_star ? sides * 2 : sides;
  double inner_r = radius * inflection;

  for (int i = 0; i < total; ++i) {
    double angle;
    double r;
    if (!is_star) {
      angle = angle_rad + i * (2.0 * M_PI / sides);
      r = radius;
    } else {
      // Even indices = outer, odd = inner
      angle = angle_rad + i * (M_PI / sides);
      r = (i % 2 == 0) ? radius : inner_r;
    }
    // Doc space is Y-down
    double px = cx + r * std::cos(angle);
    double py = cy + r * std::sin(angle);

    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = px;
    n.cy1 = py; // degenerate handles = sharp corners
    n.cx2 = px;
    n.cy2 = py;
    n.type = BezierNode::Type::Corner;
    pd.nodes.push_back(n);
  }

  return pd;
}

// ── spiral_to_path ──────────────────────────────────────────────────────────
// Logarithmic-spiral generator using the Hermite-to-Bézier parametric formula.
//
// The reference math (from Scott's research, summarised):
//
//   Logarithmic spiral:    r(θ) = a·exp(b·θ)
//   Cartesian:             x(θ) = r·cos θ,  y(θ) = r·sin θ
//   Derivative (tangent):  dx/dθ = b·r·cos θ − r·sin θ
//                          dy/dθ = b·r·sin θ + r·cos θ
//
//   For a cubic Bézier from anchor at θ0 to anchor at θ1:
//      P0 = (x(θ0), y(θ0))
//      P3 = (x(θ1), y(θ1))
//      T0 = derivative at θ0 (full magnitude, not normalised)
//      T1 = derivative at θ1
//      P1 = P0 + (Δθ/3) · T0
//      P2 = P3 − (Δθ/3) · T1
//
// This is the universal Hermite-to-Bézier conversion: matches position
// and velocity at both endpoints. Handle DIRECTION uses the true spiral
// tangent (which is tilted off perpendicular-to-radius by the spiral's
// pitch angle, automatically encoded in the derivative). Handle LENGTH
// uses |T|·Δθ/3 from the parametric derivative.
//
// History (S98): replaced the previous arc-to-Bézier formula
// `(4/3)·tan(Δθ/4)·r` with this Hermite parametric form. The old
// formula assumed a circular arc per segment and broke down on
// high-growth (conch / nautilus) spirals — visible flat sections and
// corners at the cardinal anchors. The Hermite formula handles those
// cases cleanly because the handle direction follows the true spiral
// tangent rather than perpendicular-to-radius.
//
// Parameters:
//   cx, cy      — centre in doc space (Y-down)
//   outer_r     — outer radius (at the outer tip)
//   inner_r     — inner radius (at the inner end)
//                 Must be > 0 strictly; clamped to ≥ 1% of outer_r.
//   turns       — number of full rotations
//   angle_rad   — angle of the outer tip (default -π/2 = pointing up)
//
// The growth-per-turn `k` is IMPLIED by (outer_r, inner_r, turns):
//   k = (outer_r / inner_r)^(1/turns)
// The user does not specify k directly — the existing inner/outer/turns
// inputs already determine it. (A nautilus is roughly k = 3, achieved
// by e.g. 3 turns with inner_r = outer_r/27 ≈ 3.7%.)
//
// Anchors are placed at uniform 90° steps. For extreme growth ratios
// (conchs beyond k ≈ 4-5) some mid-segment under-bow may appear; the
// remedy if needed is adaptive subdivision (smaller steps on the inner
// turns where the radius changes fast). Not done in v1.
inline PathData spiral_to_path(double cx, double cy, double outer_r,
                               double inner_r, double turns,
                               double angle_rad = -M_PI * 0.5) {
  if (turns < 0.25)
    turns = 0.25;
  if (outer_r < 1.0)
    outer_r = 1.0;
  // inner_r > 0 strict — log-spiral has no inner_r=0 (would put k=∞).
  // Clamp at 1% of outer_r as a sane floor.
  double inner_floor = outer_r * 0.01;
  inner_r = std::max(inner_floor, std::min(inner_r, outer_r * 0.95));

  PathData pd;
  pd.closed = false;

  // ── Math approach ────────────────────────────────────────────────────
  //
  // Use a clean parametric variable θ_p ∈ [0, total_angle] where:
  //   - θ_p = 0 corresponds to the inner end
  //   - θ_p = total_angle = turns·2π corresponds to the outer tip
  //
  // The spiral curve in θ_p is the writeup's standard form:
  //   r(θ_p) = a·exp(b·θ_p)
  //   x_local(θ_p) = r(θ_p)·cos(θ_p)
  //   y_local(θ_p) = r(θ_p)·sin(θ_p)
  //
  // Solving for a, b from boundary conditions:
  //   r(0)           = a            = inner_r  → a = inner_r
  //   r(total_angle) = a·exp(b·tot) = outer_r  → b = ln(outer_r/inner_r) / tot
  //
  // The derivatives:
  //   dx/dθ_p = b·r·cos(θ_p) - r·sin(θ_p)
  //   dy/dθ_p = b·r·sin(θ_p) + r·cos(θ_p)
  //
  // (No chain rule needed — θ_p IS the angle in the parametric form.
  // In this clean local frame the spiral winds CCW-outward.)
  //
  // We then transform from local (x_local, y_local) to canvas coords:
  //   1. Reflect to wind CW-outward (matches user expectation on Y-down
  //      canvas). Reflection: y → -y.
  //   2. Rotate so the OUTER TIP (at θ_p=total_angle) lands at canvas
  //      angle `angle_rad`. After CW-reflection, the outer tip in local-
  //      reflected frame sits at angle -total_angle, so the rotation is
  //      `rot = angle_rad + total_angle`.
  //   3. Translate to (cx, cy).
  //
  // The reflection y → -y also flips dy/dθ_p sign — applied below.
  // Tangent vectors transform under rotation just like points (since
  // they're vectors at a point); rotation preserves their length and
  // changes their direction by the same angle.

  double total_angle = turns * 2.0 * M_PI;
  double b = std::log(outer_r / inner_r) / total_angle;
  double a = inner_r;

  double rot = angle_rad + total_angle;
  double cos_rot = std::cos(rot);
  double sin_rot = std::sin(rot);

  // Apply reflection (y → -y) then rotation by `rot` then translate.
  auto to_canvas = [&](double lx, double ly_unreflected) {
    double ly = -ly_unreflected;  // Y-flip for CW-outward winding
    double rx = lx * cos_rot - ly * sin_rot;
    double ry = lx * sin_rot + ly * cos_rot;
    return std::pair<double, double>{cx + rx, cy + ry};
  };

  int total_segs = std::max(1, (int)std::ceil(turns * 4));

  for (int i = 0; i <= total_segs; ++i) {
    double theta_p = i * (M_PI * 0.5);
    if (theta_p > total_angle)
      theta_p = total_angle;  // clamp last partial segment

    double r = a * std::exp(b * theta_p);
    double lx = r * std::cos(theta_p);
    double ly = r * std::sin(theta_p);
    auto [px, py] = to_canvas(lx, ly);

    if (i == 0) {
      BezierNode n;
      n.x = px;
      n.y = py;
      n.cx1 = px;
      n.cy1 = py;
      n.cx2 = px;
      n.cy2 = py;
      n.type = BezierNode::Type::Smooth;
      pd.nodes.push_back(n);
      continue;
    }

    // Previous anchor.
    double theta_p0 = (i - 1) * (M_PI * 0.5);
    double r0 = a * std::exp(b * theta_p0);
    double l0x = r0 * std::cos(theta_p0);
    double l0y = r0 * std::sin(theta_p0);
    auto [p0x, p0y] = to_canvas(l0x, l0y);

    double dtheta = theta_p - theta_p0;

    // Tangent in local-unreflected frame:
    //   dx/dθ_p = b·r·cos θ_p − r·sin θ_p
    //   dy/dθ_p = b·r·sin θ_p + r·cos θ_p
    auto tangent_local = [&](double th, double rr) {
      double tx = rr * (b * std::cos(th) - std::sin(th));
      double ty = rr * (b * std::sin(th) + std::cos(th));
      return std::pair<double, double>{tx, ty};
    };
    auto [tx0_l, ty0_l_un] = tangent_local(theta_p0, r0);
    auto [tx1_l, ty1_l_un] = tangent_local(theta_p, r);

    // Apply the same reflect+rotate transform to the tangent (translation
    // doesn't affect tangents, only points).
    auto tangent_to_canvas = [&](double tx_l, double ty_l_un) {
      double ty_l = -ty_l_un;  // reflect
      double rx = tx_l * cos_rot - ty_l * sin_rot;
      double ry = tx_l * sin_rot + ty_l * cos_rot;
      return std::pair<double, double>{rx, ry};
    };
    auto [tx0, ty0] = tangent_to_canvas(tx0_l, ty0_l_un);
    auto [tx1, ty1] = tangent_to_canvas(tx1_l, ty1_l_un);

    // Hermite-to-Bézier handle placement.
    double k_factor = dtheta / 3.0;
    pd.nodes.back().cx2 = p0x + k_factor * tx0;
    pd.nodes.back().cy2 = p0y + k_factor * ty0;

    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = px - k_factor * tx1;
    n.cy1 = py - k_factor * ty1;
    n.cx2 = px;
    n.cy2 = py;
    n.type = BezierNode::Type::Smooth;
    pd.nodes.push_back(n);
  }

  return pd;
}


// ── effective_text_margins ──────────────────────────────────────────────────
// Returns the four margin values that should be applied when laying text
// into a boundary, accounting for the ownership rule documented next to
// `text_margin_*` above. The rule:
//
//   1. If the boundary has any non-zero margin → boundary owns them all.
//      Read all four from the boundary; ignore the text's.
//
//   2. Otherwise (boundary is zero on all four) → fall back to the
//      text's margins. This handles legacy paired-sibling text from
//      before the TextBox migration.
//
// Both sides default to zero, which is also the "edge is the text edge"
// state — so a fully-zero pair correctly returns all zeros from either
// branch.
//
// Pass either side null safely — a null is treated as "no margins on
// that side." Callers in TextCursor and Canvas_draw always have both
// pointers, but the defensive shape protects against unexpected nullptr
// without bringing the layout function down.
struct EffectiveTextMargins {
  double top = 0.0;
  double bottom = 0.0;
  double left = 0.0;
  double right = 0.0;
};

inline EffectiveTextMargins effective_text_margins(const SceneNode* text,
                                                   const SceneNode* boundary) {
  EffectiveTextMargins m;
  if (boundary) {
    bool boundary_owns = boundary->text_margin_top    != 0.0 ||
                         boundary->text_margin_bottom != 0.0 ||
                         boundary->text_margin_left   != 0.0 ||
                         boundary->text_margin_right  != 0.0;
    if (boundary_owns) {
      m.top    = boundary->text_margin_top;
      m.bottom = boundary->text_margin_bottom;
      m.left   = boundary->text_margin_left;
      m.right  = boundary->text_margin_right;
      return m;
    }
  }
  if (text) {
    m.top    = text->text_margin_top;
    m.bottom = text->text_margin_bottom;
    m.left   = text->text_margin_left;
    m.right  = text->text_margin_right;
  }
  return m;
}

// Legacy compat — GlyphObject alias so old code compiles during transition
// Remove once all call sites are updated.
using GlyphObject = SceneNode;

} // namespace Curvz
